// Copyright (c) 2016- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

// Additionally, Common/Vulkan/* , including this file, are also licensed
// under the public domain.

#include "Common/Math/math_util.h"

#include "Common/Log.h"
#include "Common/TimeUtil.h"
#include "Common/GPU/Vulkan/VulkanMemory.h"

using namespace PPSSPP_VK;

VulkanPushBuffer::VulkanPushBuffer(VulkanContext *vulkan, size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryPropertyMask)
		: vulkan_(vulkan), memoryPropertyMask_(memoryPropertyMask), size_(size), usage_(usage) {
	bool res = AddBuffer();
	_assert_(res);
}

VulkanPushBuffer::~VulkanPushBuffer() {
	_assert_(buffers_.empty());
}

bool VulkanPushBuffer::AddBuffer() {
	BufInfo info;
	VkDevice device = vulkan_->GetDevice();

	VkBufferCreateInfo b{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	b.size = size_;
	b.flags = 0;
	b.usage = usage_;
	b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	b.queueFamilyIndexCount = 0;
	b.pQueueFamilyIndices = nullptr;

	VkResult res = vkCreateBuffer(device, &b, nullptr, &info.buffer);
	if (VK_SUCCESS != res) {
		_assert_msg_(false, "vkCreateBuffer failed! result=%d", (int)res);
		return false;
	}

	// Get the buffer memory requirements. None of this can be cached!
	VkMemoryRequirements reqs;
	vkGetBufferMemoryRequirements(device, info.buffer, &reqs);

	// Okay, that's the buffer. Now let's allocate some memory for it.
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = reqs.size;
	vulkan_->MemoryTypeFromProperties(reqs.memoryTypeBits, memoryPropertyMask_, &alloc.memoryTypeIndex);

	res = vkAllocateMemory(device, &alloc, nullptr, &info.deviceMemory);
	if (VK_SUCCESS != res) {
		_assert_msg_(false, "vkAllocateMemory failed! size=%d result=%d", (int)reqs.size, (int)res);
		vkDestroyBuffer(device, info.buffer, nullptr);
		return false;
	}
	res = vkBindBufferMemory(device, info.buffer, info.deviceMemory, 0);
	if (VK_SUCCESS != res) {
		ERROR_LOG(G3D, "vkBindBufferMemory failed! result=%d", (int)res);
		vkFreeMemory(device, info.deviceMemory, nullptr);
		vkDestroyBuffer(device, info.buffer, nullptr);
		return false;
	}

	buffers_.push_back(info);
	buf_ = buffers_.size() - 1;
	return true;
}

void VulkanPushBuffer::Destroy(VulkanContext *vulkan) {
	for (BufInfo &info : buffers_) {
		vulkan->Delete().QueueDeleteBuffer(info.buffer);
		vulkan->Delete().QueueDeleteDeviceMemory(info.deviceMemory);
	}
	buffers_.clear();
}

void VulkanPushBuffer::NextBuffer(size_t minSize) {
	// First, unmap the current memory.
	if (memoryPropertyMask_ & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		Unmap();

	buf_++;
	if (buf_ >= buffers_.size() || minSize > size_) {
		// Before creating the buffer, adjust to the new size_ if necessary.
		while (size_ < minSize) {
			size_ <<= 1;
		}

		bool res = AddBuffer();
		_assert_(res);
		if (!res) {
			// Let's try not to crash at least?
			buf_ = 0;
		}
	}

	// Now, move to the next buffer and map it.
	offset_ = 0;
	if (memoryPropertyMask_ & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		Map();
}

void VulkanPushBuffer::Defragment(VulkanContext *vulkan) {
	if (buffers_.size() <= 1) {
		return;
	}

	// Okay, we have more than one.  Destroy them all and start over with a larger one.
	size_t newSize = size_ * buffers_.size();
	Destroy(vulkan);

	size_ = newSize;
	bool res = AddBuffer();
	_assert_(res);
}

size_t VulkanPushBuffer::GetTotalSize() const {
	size_t sum = 0;
	if (buffers_.size() > 1)
		sum += size_ * (buffers_.size() - 1);
	sum += offset_;
	return sum;
}

void VulkanPushBuffer::Map() {
	_dbg_assert_(!writePtr_);
	VkResult res = vkMapMemory(vulkan_->GetDevice(), buffers_[buf_].deviceMemory, 0, size_, 0, (void **)(&writePtr_));
	_dbg_assert_(writePtr_);
	_assert_(VK_SUCCESS == res);
}

void VulkanPushBuffer::Unmap() {
	_dbg_assert_msg_(writePtr_ != nullptr, "VulkanPushBuffer::Unmap: writePtr_ null here means we have a bug (map/unmap mismatch)");
	if (!writePtr_)
		return;

	if ((memoryPropertyMask_ & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
		VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.offset = 0;
		range.size = offset_;
		range.memory = buffers_[buf_].deviceMemory;
		vkFlushMappedMemoryRanges(vulkan_->GetDevice(), 1, &range);
	}

	vkUnmapMemory(vulkan_->GetDevice(), buffers_[buf_].deviceMemory);
	writePtr_ = nullptr;
}

VulkanDeviceAllocator::VulkanDeviceAllocator(VulkanContext *vulkan, size_t minSlabSize, size_t maxSlabSize)
	: vulkan_(vulkan), minSlabSize_(minSlabSize), maxSlabSize_(maxSlabSize) {
	_assert_((minSlabSize_ & (SLAB_GRAIN_SIZE - 1)) == 0);
}

VulkanDeviceAllocator::~VulkanDeviceAllocator() {
	_assert_(destroyed_);
	_assert_(slabs_.empty());
}

void VulkanDeviceAllocator::Destroy() {
	for (Slab &slab : slabs_) {
		// Did anyone forget to free?
		for (auto pair : slab.allocSizes) {
			int slabUsage = slab.usage[pair.first];
			// If it's not 2 (queued), there's a leak.
			// If it's zero, it means allocSizes is somehow out of sync.
			if (slabUsage == 1) {
				ERROR_LOG(G3D, "VulkanDeviceAllocator detected memory leak of size %d", (int)pair.second);
			} else {
				_dbg_assert_msg_(slabUsage == 2, "Destroy: slabUsage has unexpected value %d", slabUsage);
			}
		}

		_assert_(slab.deviceMemory);
		vulkan_->Delete().QueueDeleteDeviceMemory(slab.deviceMemory);
	}
	slabs_.clear();
	destroyed_ = true;
}

size_t VulkanDeviceAllocator::Allocate(const VkMemoryRequirements &reqs, VkDeviceMemory *deviceMemory, const char *tag) {
	_assert_(!destroyed_);
	uint32_t memoryTypeIndex;
	bool pass = vulkan_->MemoryTypeFromProperties(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex);
	if (!pass) {
		ERROR_LOG(G3D, "Failed to pick an appropriate memory type (req: %08x)", reqs.memoryTypeBits);
		return ALLOCATE_FAILED;
	}

	size_t size = reqs.size;

	size_t align = reqs.alignment <= SLAB_GRAIN_SIZE ? 1 : (size_t)(reqs.alignment >> SLAB_GRAIN_SHIFT);
	size_t blocks = (size_t)((size + SLAB_GRAIN_SIZE - 1) >> SLAB_GRAIN_SHIFT);

	const size_t numSlabs = slabs_.size();
	for (size_t i = 0; i < numSlabs; ++i) {
		// We loop starting at the last successful allocation.
		// This helps us "creep forward", and also spend less time allocating.
		const size_t actualSlab = (lastSlab_ + i) % numSlabs;
		Slab &slab = slabs_[actualSlab];
		if (slab.memoryTypeIndex != memoryTypeIndex)
			continue;
		size_t start = slab.nextFree;

		while (start < slab.usage.size()) {
			start = (start + align - 1) & ~(align - 1);
			if (AllocateFromSlab(slab, start, blocks, tag)) {
				// Allocated?  Great, let's return right away.
				*deviceMemory = slab.deviceMemory;
				lastSlab_ = actualSlab;
				return start << SLAB_GRAIN_SHIFT;
			}
		} 
	}

	// Okay, we couldn't fit it into any existing slabs.  We need a new one.
	if (!AllocateSlab(size, memoryTypeIndex)) {
		return ALLOCATE_FAILED;
	}

	// Guaranteed to be the last one, unless it failed to allocate.
	Slab &slab = slabs_[slabs_.size() - 1];
	size_t start = 0;
	if (AllocateFromSlab(slab, start, blocks, tag)) {
		*deviceMemory = slab.deviceMemory;
		lastSlab_ = slabs_.size() - 1;
		return start << SLAB_GRAIN_SHIFT;
	}

	// Somehow... we're out of space.  Darn.
	return ALLOCATE_FAILED;
}

bool VulkanDeviceAllocator::AllocateFromSlab(Slab &slab, size_t &start, size_t blocks, const char *tag) {
	_assert_(!destroyed_);

	if (start + blocks > slab.usage.size()) {
		start = slab.usage.size();
		return false;
	}

	for (size_t i = 0; i < blocks; ++i) {
		if (slab.usage[start + i]) {
			// If we just ran into one, there's probably an allocation size.
			auto it = slab.allocSizes.find(start + i);
			if (it != slab.allocSizes.end()) {
				start += i + it->second;
			} else {
				// We don't know how big it is, so just skip to the next one.
				start += i + 1;
			}
			return false;
		}
	}

	// Okay, this run is good.  Actually mark it.
	for (size_t i = 0; i < blocks; ++i) {
		slab.usage[start + i] = 1;
	}
	slab.nextFree = start + blocks;
	if (slab.nextFree >= slab.usage.size()) {
		slab.nextFree = 0;
	}

	// Remember the size so we can free.
	slab.allocSizes[start] = blocks;
	slab.tags[start] = { time_now_d(), 0.0, tag };
	slab.totalUsage += blocks;
	return true;
}

int VulkanDeviceAllocator::ComputeUsagePercent() const {
	int blockSum = 0;
	int blocksUsed = 0;
	for (size_t i = 0; i < slabs_.size(); i++) {
		blockSum += (int)slabs_[i].usage.size();
		for (size_t j = 0; j < slabs_[i].usage.size(); j++) {
			blocksUsed += slabs_[i].usage[j] != 0 ? 1 : 0;
		}
	}
	return blockSum == 0 ? 0 : 100 * blocksUsed / blockSum;
}

std::vector<uint8_t> VulkanDeviceAllocator::GetSlabUsage(int slabIndex) const {
	if (slabIndex < 0 || slabIndex >= (int)slabs_.size())
		return std::vector<uint8_t>();
	const Slab &slab = slabs_[slabIndex];
	return slab.usage;
}

void VulkanDeviceAllocator::DoTouch(VkDeviceMemory deviceMemory, size_t offset) {
	size_t start = offset >> SLAB_GRAIN_SHIFT;
	bool found = false;
	for (Slab &slab : slabs_) {
		if (slab.deviceMemory != deviceMemory) {
			continue;
		}

		auto it = slab.tags.find(start);
		if (it != slab.tags.end()) {
			it->second.touched = time_now_d();
			found = true;
		}
	}

	_assert_msg_(found, "Failed to find allocation to touch - use after free?");
}

void VulkanDeviceAllocator::Free(VkDeviceMemory deviceMemory, size_t offset) {
	_assert_(!destroyed_);

	_assert_msg_(!slabs_.empty(), "No slabs - can't be anything to free! double-freed?");

	// First, let's validate.  This will allow stack traces to tell us when frees are bad.
	size_t start = offset >> SLAB_GRAIN_SHIFT;
	bool found = false;
	for (Slab &slab : slabs_) {
		if (slab.deviceMemory != deviceMemory) {
			continue;
		}

		auto it = slab.allocSizes.find(start);
		_assert_msg_(it != slab.allocSizes.end(), "Double free?");
		// This means a double free, while queued to actually free.
		_assert_msg_(slab.usage[start] == 1, "Double free when queued to free!");

		// Mark it as "free in progress".
		slab.usage[start] = 2;
		found = true;
		break;
	}

	// Wrong deviceMemory even?  Maybe it was already decimated, but that means a double-free.
	_assert_msg_(found, "Failed to find allocation to free! Double-freed?");

	// Okay, now enqueue.  It's valid.
	FreeInfo *info = new FreeInfo(this, deviceMemory, offset);
	// Dispatches a call to ExecuteFree on the next delete round.
	vulkan_->Delete().QueueCallback(&DispatchFree, info);
}

void VulkanDeviceAllocator::ExecuteFree(FreeInfo *userdata) {
	if (destroyed_) {
		// We already freed this, and it's been validated.
		delete userdata;
		return;
	}

	VkDeviceMemory deviceMemory = userdata->deviceMemory;
	size_t offset = userdata->offset;

	// Revalidate in case something else got freed and made things inconsistent.
	size_t start = offset >> SLAB_GRAIN_SHIFT;
	bool found = false;
	for (Slab &slab : slabs_) {
		if (slab.deviceMemory != deviceMemory) {
			continue;
		}

		auto it = slab.allocSizes.find(start);
		if (it != slab.allocSizes.end()) {
			size_t size = it->second;
			for (size_t i = 0; i < size; ++i) {
				slab.usage[start + i] = 0;
			}
			slab.allocSizes.erase(it);
			slab.totalUsage -= size;

			// Allow reusing.
			if (slab.nextFree > start) {
				slab.nextFree = start;
			}
		} else {
			// Ack, a double free?
			_assert_msg_(false, "Double free? Block missing at offset %d", (int)userdata->offset);
		}
		auto itTag = slab.tags.find(start);
		if (itTag != slab.tags.end()) {
			slab.tags.erase(itTag);
		}
		found = true;
		break;
	}

	// Wrong deviceMemory even?  Maybe it was already decimated, but that means a double-free.
	_assert_msg_(found, "ExecuteFree: Block not found (offset %d)", (int)offset);
	delete userdata;
}

bool VulkanDeviceAllocator::AllocateSlab(VkDeviceSize minBytes, int memoryTypeIndex) {
	_assert_(!destroyed_);
	if (!slabs_.empty() && minSlabSize_ < maxSlabSize_) {
		// We're allocating an additional slab, so rachet up its size.
		// TODO: Maybe should not do this when we are allocating a new slab due to memoryTypeIndex not matching?
		minSlabSize_ <<= 1;
	}

	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = minSlabSize_;
	alloc.memoryTypeIndex = memoryTypeIndex;

	while (alloc.allocationSize < minBytes) {
		alloc.allocationSize <<= 1;
	}

	VkDeviceMemory deviceMemory;
	VkResult res = vkAllocateMemory(vulkan_->GetDevice(), &alloc, NULL, &deviceMemory);
	if (res != VK_SUCCESS) {
		// If it's something else, we used it wrong?
		_assert_(res == VK_ERROR_OUT_OF_HOST_MEMORY || res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_TOO_MANY_OBJECTS);
		// Okay, so we ran out of memory.
		return false;
	}

	slabs_.resize(slabs_.size() + 1);
	Slab &slab = slabs_[slabs_.size() - 1];
	slab.memoryTypeIndex = memoryTypeIndex;
	slab.deviceMemory = deviceMemory;
	slab.usage.resize((size_t)(alloc.allocationSize >> SLAB_GRAIN_SHIFT));

	return true;
}

void VulkanDeviceAllocator::ReportOldUsage() {
	double now = time_now_d();
	static const double OLD_AGE = 10.0;
	for (size_t i = 0; i < slabs_.size(); ++i) {
		const auto &slab = slabs_[i];

		bool hasOldAllocs = false;
		for (auto &it : slab.tags) {
			const auto info = it.second;
			double touchedAge = now - info.touched;
			if (touchedAge >= OLD_AGE) {
				hasOldAllocs = true;
				break;
			}
		}

		if (hasOldAllocs) {
			NOTICE_LOG(G3D, "Slab %d usage:", (int)i);
			for (auto &it : slab.tags) {
				const auto info = it.second;

				double createAge = now - info.created;
				double touchedAge = now - info.touched;
				NOTICE_LOG(G3D, "  * %s (created %fs ago, used %fs ago)", info.tag, createAge, touchedAge);
			}
		}
	}
}

void VulkanDeviceAllocator::Decimate() {
	_assert_(!destroyed_);
	bool foundFree = false;

	if (TRACK_TOUCH) {
		ReportOldUsage();
	}

	for (size_t i = 0; i < slabs_.size(); ++i) {
		// Go backwards.  This way, we keep the largest free slab.
		// We do this here (instead of the for) since size_t is unsigned.
		size_t index = slabs_.size() - i - 1;
		auto &slab = slabs_[index];

		if (!slab.allocSizes.empty()) {
			size_t usagePercent = 100 * slab.totalUsage / slab.usage.size();
			size_t freeNextPercent = 100 * slab.nextFree / slab.usage.size();

			// This may mean we're going to leave an allocation hanging.  Reset nextFree instead.
			if (freeNextPercent >= 100 - usagePercent) {
				size_t newFree = 0;
				while (newFree < slab.usage.size()) {
					auto it = slab.allocSizes.find(newFree);
					if (it == slab.allocSizes.end()) {
						break;
					}

					newFree += it->second;
				}

				slab.nextFree = newFree;
			}
			continue;
		}

		if (!foundFree) {
			// Let's allow one free slab, so we have room.
			foundFree = true;
			continue;
		}

		// Okay, let's free this one up.
		vulkan_->Delete().QueueDeleteDeviceMemory(slab.deviceMemory);
		slabs_.erase(slabs_.begin() + index);

		// Let's check the next one, which is now in this same slot.
		--i;
	}
}
