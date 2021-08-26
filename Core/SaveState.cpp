// Copyright (c) 2012- PPSSPP Project.

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

#include <algorithm>
#include <vector>
#include <thread>
#include <mutex>

#include "Common/Data/Text/I18n.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Data/Text/Parsers.h"

#include "Common/File/FileUtil.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/StringUtils.h"
#include "Common/TimeUtil.h"

#include "Core/SaveState.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Host.h"
#include "Core/Screenshot.h"
#include "Core/System.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceDisplay.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/JitCommon/JitBlockCache.h"
#include "HW/MemoryStick.h"
#include "GPU/GPUState.h"

#ifndef MOBILE_DEVICE
#include "Core/AVIDump.h"
#include "Core/HLE/__sceAudio.h"
#endif

// Slot number is visual only, -2 will display special message
constexpr int LOAD_UNDO_SLOT = -2;

namespace SaveState
{
	struct SaveStart
	{
		void DoState(PointerWrap &p);
	};

	enum OperationType
	{
		SAVESTATE_SAVE,
		SAVESTATE_LOAD,
		SAVESTATE_VERIFY,
		SAVESTATE_REWIND,
		SAVESTATE_SAVE_SCREENSHOT,
	};

	struct Operation
	{
		// The slot number is for visual purposes only. Set to -1 for operations where we don't display a message for example.
		Operation(OperationType t, const Path &f, int slot_, Callback cb, void *cbUserData_)
			: type(t), filename(f), callback(cb), slot(slot_), cbUserData(cbUserData_)
		{
		}

		OperationType type;
		Path filename;
		Callback callback;
		int slot;
		void *cbUserData;
	};

	CChunkFileReader::Error SaveToRam(std::vector<u8> &data) {
		SaveStart state;
		size_t sz = CChunkFileReader::MeasurePtr(state);
		if (data.size() < sz)
			data.resize(sz);
		return CChunkFileReader::SavePtr(&data[0], state, sz);
	}

	CChunkFileReader::Error LoadFromRam(std::vector<u8> &data, std::string *errorString) {
		SaveStart state;
		return CChunkFileReader::LoadPtr(&data[0], state, errorString);
	}

	struct StateRingbuffer
	{
		StateRingbuffer(int size) : first_(0), next_(0), size_(size), base_(-1)
		{
			states_.resize(size);
			baseMapping_.resize(size);
		}

		CChunkFileReader::Error Save()
		{
			std::lock_guard<std::mutex> guard(lock_);

			int n = next_++ % size_;
			if ((next_ % size_) == first_)
				++first_;

			static std::vector<u8> buffer;
			std::vector<u8> *compressBuffer = &buffer;
			CChunkFileReader::Error err;

			if (base_ == -1 || ++baseUsage_ > BASE_USAGE_INTERVAL)
			{
				base_ = (base_ + 1) % ARRAY_SIZE(bases_);
				baseUsage_ = 0;
				err = SaveToRam(bases_[base_]);
				// Let's not bother savestating twice.
				compressBuffer = &bases_[base_];
			}
			else
				err = SaveToRam(buffer);

			if (err == CChunkFileReader::ERROR_NONE)
				ScheduleCompress(&states_[n], compressBuffer, &bases_[base_]);
			else
				states_[n].clear();
			baseMapping_[n] = base_;
			return err;
		}

		CChunkFileReader::Error Restore(std::string *errorString)
		{
			std::lock_guard<std::mutex> guard(lock_);

			// No valid states left.
			if (Empty())
				return CChunkFileReader::ERROR_BAD_FILE;

			int n = (--next_ + size_) % size_;
			if (states_[n].empty())
				return CChunkFileReader::ERROR_BAD_FILE;

			static std::vector<u8> buffer;
			LockedDecompress(buffer, states_[n], bases_[baseMapping_[n]]);
			return LoadFromRam(buffer, errorString);
		}

		void ScheduleCompress(std::vector<u8> *result, const std::vector<u8> *state, const std::vector<u8> *base)
		{
			if (compressThread_.joinable())
				compressThread_.join();
			compressThread_ = std::thread([=]{
				SetCurrentThreadName("SaveStateCompress");
				Compress(*result, *state, *base);
			});
		}

		void Compress(std::vector<u8> &result, const std::vector<u8> &state, const std::vector<u8> &base)
		{
			std::lock_guard<std::mutex> guard(lock_);
			// Bail if we were cleared before locking.
			if (first_ == 0 && next_ == 0)
				return;

			result.clear();
			for (size_t i = 0; i < state.size(); i += BLOCK_SIZE)
			{
				int blockSize = std::min(BLOCK_SIZE, (int)(state.size() - i));
				if (i + blockSize > base.size() || memcmp(&state[i], &base[i], blockSize) != 0)
				{
					result.push_back(1);
					result.insert(result.end(), state.begin() + i, state.begin() +i + blockSize);
				}
				else
					result.push_back(0);
			}
		}

		void LockedDecompress(std::vector<u8> &result, const std::vector<u8> &compressed, const std::vector<u8> &base)
		{
			result.clear();
			result.reserve(base.size());
			auto basePos = base.begin();
			for (size_t i = 0; i < compressed.size(); )
			{
				if (compressed[i] == 0)
				{
					++i;
					int blockSize = std::min(BLOCK_SIZE, (int)(base.size() - result.size()));
					result.insert(result.end(), basePos, basePos + blockSize);
					basePos += blockSize;
				}
				else
				{
					++i;
					int blockSize = std::min(BLOCK_SIZE, (int)(compressed.size() - i));
					result.insert(result.end(), compressed.begin() + i, compressed.begin() + i + blockSize);
					i += blockSize;
					basePos += blockSize;
				}
			}
		}

		void Clear()
		{
			if (compressThread_.joinable())
				compressThread_.join();

			// This lock is mainly for shutdown.
			std::lock_guard<std::mutex> guard(lock_);
			first_ = 0;
			next_ = 0;
		}

		bool Empty() const
		{
			return next_ == first_;
		}

		static const int BLOCK_SIZE;
		// TODO: Instead, based on size of compressed state?
		static const int BASE_USAGE_INTERVAL;

		typedef std::vector<u8> StateBuffer;

		int first_;
		int next_;
		int size_;

		std::vector<StateBuffer> states_;
		StateBuffer bases_[2];
		std::vector<int> baseMapping_;
		std::mutex lock_;
		std::thread compressThread_;

		int base_;
		int baseUsage_;
	};

	static bool needsProcess = false;
	static bool needsRestart = false;
	static std::vector<Operation> pending;
	static std::mutex mutex;
	static int screenshotFailures = 0;
	static bool hasLoadedState = false;
	static const int STALE_STATE_USES = 2;
	// 4 hours of total gameplay since the virtual PSP started the game.
	static const u64 STALE_STATE_TIME = 4 * 3600 * 1000000ULL;
	static int saveStateGeneration = 0;
	static int saveDataGeneration = 0;
	static int lastSaveDataGeneration = 0;
	static std::string saveStateInitialGitVersion = "";

	// TODO: Should this be configurable?
	static const int REWIND_NUM_STATES = 20;
	static const int SCREENSHOT_FAILURE_RETRIES = 15;
	static StateRingbuffer rewindStates(REWIND_NUM_STATES);
	// TODO: Any reason for this to be configurable?
	const static float rewindMaxWallFrequency = 1.0f;
	static double rewindLastTime = 0.0f;
	const int StateRingbuffer::BLOCK_SIZE = 8192;
	const int StateRingbuffer::BASE_USAGE_INTERVAL = 15;

	void SaveStart::DoState(PointerWrap &p)
	{
		auto s = p.Section("SaveStart", 1, 2);
		if (!s)
			return;

		if (s >= 2) {
			// This only increments on save, of course.
			++saveStateGeneration;
			Do(p, saveStateGeneration);
			// This saves the first git version to create this save state (or generation of save states.)
			if (saveStateInitialGitVersion.empty())
				saveStateInitialGitVersion = PPSSPP_GIT_VERSION;
			Do(p, saveStateInitialGitVersion);
		} else {
			saveStateGeneration = 1;
		}
		if (s >= 3) {
			// Keep track of savedata (not save states) too.
			Do(p, saveDataGeneration);
		} else {
			saveDataGeneration = 0;
		}

		// Gotta do CoreTiming first since we'll restore into it.
		CoreTiming::DoState(p);

		// Memory is a bit tricky when jit is enabled, since there's emuhacks in it.
		auto savedReplacements = SaveAndClearReplacements();
		if (MIPSComp::jit && p.mode == p.MODE_WRITE)
		{
			std::vector<u32> savedBlocks;
			savedBlocks = MIPSComp::jit->SaveAndClearEmuHackOps();
			Memory::DoState(p);
			MIPSComp::jit->RestoreSavedEmuHackOps(savedBlocks);
		}
		else
			Memory::DoState(p);
		RestoreSavedReplacements(savedReplacements);

		MemoryStick_DoState(p);
		currentMIPS->DoState(p);
		HLEDoState(p);
		__KernelDoState(p);
		// Kernel object destructors might close open files, so do the filesystem last.
		pspFileSystem.DoState(p);
	}

	void Enqueue(SaveState::Operation op)
	{
		std::lock_guard<std::mutex> guard(mutex);
		pending.push_back(op);

		// Don't actually run it until next frame.
		// It's possible there might be a duplicate but it won't hurt us.
		needsProcess = true;
		Core_UpdateSingleStep();
	}

	void Load(const Path &filename, int slot, Callback callback, void *cbUserData)
	{
		if (coreState == CoreState::CORE_RUNTIME_ERROR)
			Core_EnableStepping(true);
		Enqueue(Operation(SAVESTATE_LOAD, filename, slot, callback, cbUserData));
	}

	void Save(const Path &filename, int slot, Callback callback, void *cbUserData)
	{
		if (coreState == CoreState::CORE_RUNTIME_ERROR)
			Core_EnableStepping(true);
		Enqueue(Operation(SAVESTATE_SAVE, filename, slot, callback, cbUserData));
	}

	void Verify(Callback callback, void *cbUserData)
	{
		Enqueue(Operation(SAVESTATE_VERIFY, Path(), -1, callback, cbUserData));
	}

	void Rewind(Callback callback, void *cbUserData)
	{
		if (coreState == CoreState::CORE_RUNTIME_ERROR)
			Core_EnableStepping(true);
		Enqueue(Operation(SAVESTATE_REWIND, Path(), -1, callback, cbUserData));
	}

	void SaveScreenshot(const Path &filename, Callback callback, void *cbUserData)
	{
		Enqueue(Operation(SAVESTATE_SAVE_SCREENSHOT, filename, -1, callback, cbUserData));
	}

	bool CanRewind()
	{
		return !rewindStates.Empty();
	}

	// Slot utilities

	std::string AppendSlotTitle(const std::string &filename, const std::string &title) {
		char slotChar = 0;
		auto detectSlot = [&](const std::string &ext) {
			if (!endsWith(filename, std::string(".") + ext)) {
				return false;
			}

			// Usually these are slots, let's check the slot # after the last '_'.
			size_t slotNumPos = filename.find_last_of('_');
			if (slotNumPos == filename.npos) {
				return false;
			}

			const size_t extLength = ext.length() + 1;
			// If we take out the extension, '_', etc. we should be left with only a single digit.
			if (slotNumPos + 1 + extLength != filename.length() - 1) {
				return false;
			}

			slotChar = filename[slotNumPos + 1];
			if (slotChar < '0' || slotChar > '8') {
				return false;
			}

			// Change from zero indexed to human friendly.
			slotChar++;
			return true;
		};

		if (detectSlot(STATE_EXTENSION)) {
			return StringFromFormat("%s (%c)", title.c_str(), slotChar);
		}
		if (detectSlot(UNDO_STATE_EXTENSION)) {
			auto sy = GetI18NCategory("System");
			// Allow the number to be positioned where it makes sense.
			std::string undo = sy->T("undo %c");
			return title + " (" + StringFromFormat(undo.c_str(), slotChar) + ")";
		}

		// Couldn't detect, use the filename.
		return title + " (" + filename + ")";
	}

	std::string GetTitle(const Path &filename) {
		std::string title;
		if (CChunkFileReader::GetFileTitle(filename, &title) == CChunkFileReader::ERROR_NONE) {
			if (title.empty()) {
				return filename.GetFilename();
			}

			return AppendSlotTitle(filename.GetFilename(), title);
		}

		// The file can't be loaded - let's note that.
		auto sy = GetI18NCategory("System");
		return filename.GetFilename() + " " + sy->T("(broken)");
	}

	std::string GenerateFullDiscId(const Path &gameFilename) {
		std::string discId = g_paramSFO.GetValueString("DISC_ID");
		std::string discVer = g_paramSFO.GetValueString("DISC_VERSION");
		if (discId.empty()) {
			discId = g_paramSFO.GenerateFakeID();
			discVer = "1.00";
		}
		return StringFromFormat("%s_%s", discId.c_str(), discVer.c_str());
	}

	Path GenerateSaveSlotFilename(const Path &gameFilename, int slot, const char *extension)
	{
		std::string filename = StringFromFormat("%s_%d.%s", GenerateFullDiscId(gameFilename).c_str(), slot, extension);
		return GetSysDirectory(DIRECTORY_SAVESTATE) / filename;
	}

	int GetCurrentSlot()
	{
		return g_Config.iCurrentStateSlot;
	}

	void NextSlot()
	{
		g_Config.iCurrentStateSlot = (g_Config.iCurrentStateSlot + 1) % NUM_SLOTS;
	}

	static void DeleteIfExists(const Path &fn) {
		// Just avoiding error messages.
		if (File::Exists(fn)) {
			File::Delete(fn);
		}
	}

	static void RenameIfExists(const Path &from, const Path &to) {
		if (File::Exists(from)) {
			File::Rename(from, to);
		}
	}

	static void SwapIfExists(const Path &from, const Path &to) {
		Path temp = from.WithExtraExtension(".tmp");
		if (File::Exists(from)) {
			File::Rename(from, temp);
			File::Rename(to, from);
			File::Rename(temp, to);
		}
	}

	void LoadSlot(const Path &gameFilename, int slot, Callback callback, void *cbUserData)
	{
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, STATE_EXTENSION);
		if (!fn.empty()) {
			// This add only 1 extra state, should we just always enable it?
			if (g_Config.bEnableStateUndo) {
				Path backup = GetSysDirectory(DIRECTORY_SAVESTATE) / LOAD_UNDO_NAME;
				
				auto saveCallback = [=](Status status, const std::string &message, void *data) {
					if (status != Status::FAILURE) {
						DeleteIfExists(backup);
						File::Rename(backup.WithExtraExtension(".tmp"), backup);
						g_Config.sStateLoadUndoGame = GenerateFullDiscId(gameFilename);
						g_Config.Save("Saving config for savestate last load undo");
					} else {
						ERROR_LOG(SAVESTATE, "Saving load undo state failed: %s", message.c_str());
					}
					Load(fn, slot, callback, cbUserData);
				};

				if (!backup.empty()) {
					Save(backup.WithExtraExtension(".tmp"), LOAD_UNDO_SLOT, saveCallback, cbUserData);
				} else {
					ERROR_LOG(SAVESTATE, "Saving load undo state failed. Error in the file system.");
					Load(fn, slot, callback, cbUserData);
				}
			} else {
				Load(fn, slot, callback, cbUserData);
			}
		} else {
			auto sy = GetI18NCategory("System");
			if (callback)
				callback(Status::FAILURE, sy->T("Failed to load state. Error in the file system."), cbUserData);
		}
	}

	bool UndoLoad(const Path &gameFilename, Callback callback, void *cbUserData)
	{
		if (g_Config.sStateLoadUndoGame != GenerateFullDiscId(gameFilename)) {
			auto sy = GetI18NCategory("System");
			if (callback)
				callback(Status::FAILURE, sy->T("Error: load undo state is from a different game"), cbUserData);
			return false;
		}

		Path fn = GetSysDirectory(DIRECTORY_SAVESTATE) / LOAD_UNDO_NAME;
		if (!fn.empty()) {
			Load(fn, LOAD_UNDO_SLOT, callback, cbUserData);
			return true;
		} else {
			auto sy = GetI18NCategory("System");
			if (callback)
				callback(Status::FAILURE, sy->T("Failed to load state for load undo. Error in the file system."), cbUserData);
			return false;
		}
	}

	void SaveSlot(const Path &gameFilename, int slot, Callback callback, void *cbUserData)
	{
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, STATE_EXTENSION);
		Path shot = GenerateSaveSlotFilename(gameFilename, slot, SCREENSHOT_EXTENSION);
		Path fnUndo = GenerateSaveSlotFilename(gameFilename, slot, UNDO_STATE_EXTENSION);
		Path shotUndo = GenerateSaveSlotFilename(gameFilename, slot, UNDO_SCREENSHOT_EXTENSION);
		if (!fn.empty()) {
			auto renameCallback = [=](Status status, const std::string &message, void *data) {
				if (status != Status::FAILURE) {
					if (g_Config.bEnableStateUndo) {
						DeleteIfExists(fnUndo);
						RenameIfExists(fn, fnUndo);
						g_Config.sStateUndoLastSaveGame = GenerateFullDiscId(gameFilename);
						g_Config.iStateUndoLastSaveSlot = slot;
						g_Config.Save("Saving config for savestate last save undo");
					} else {
						DeleteIfExists(fn);
					}
					File::Rename(fn.WithExtraExtension(".tmp"), fn);
				}
				if (callback) {
					callback(status, message, data);
				}
			};
			// Let's also create a screenshot.
			if (g_Config.bEnableStateUndo) {
				DeleteIfExists(shotUndo);
				RenameIfExists(shot, shotUndo);
			}
			SaveScreenshot(shot, Callback(), 0);
			Save(fn.WithExtraExtension(".tmp"), slot, renameCallback, cbUserData);
		} else {
			auto sy = GetI18NCategory("System");
			if (callback)
				callback(Status::FAILURE, sy->T("Failed to save state. Error in the file system."), cbUserData);
		}
	}

	bool UndoSaveSlot(const Path &gameFilename, int slot) {
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, STATE_EXTENSION);
		Path shot = GenerateSaveSlotFilename(gameFilename, slot, SCREENSHOT_EXTENSION);
		Path fnUndo = GenerateSaveSlotFilename(gameFilename, slot, UNDO_STATE_EXTENSION);
		Path shotUndo = GenerateSaveSlotFilename(gameFilename, slot, UNDO_SCREENSHOT_EXTENSION);

		// Do nothing if there's no undo.
		if (File::Exists(fnUndo)) {
			// Swap them so they can undo again to redo.  Mistakes happen.
			SwapIfExists(shotUndo, shot);
			SwapIfExists(fnUndo, fn);
			return true;
		}

		return false;
	}


	bool UndoLastSave(const Path &gameFilename) {
		if (g_Config.sStateUndoLastSaveGame != GenerateFullDiscId(gameFilename))
			return false;

		return UndoSaveSlot(gameFilename, g_Config.iStateUndoLastSaveSlot);
	}

	bool HasSaveInSlot(const Path &gameFilename, int slot)
	{
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, STATE_EXTENSION);
		return File::Exists(fn);
	}

	bool HasUndoSaveInSlot(const Path &gameFilename, int slot)
	{
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, UNDO_STATE_EXTENSION);
		return File::Exists(fn);
	}

	bool HasUndoLastSave(const Path &gameFilename) 
	{
		if (g_Config.sStateUndoLastSaveGame != GenerateFullDiscId(gameFilename))
			return false;

		return HasUndoSaveInSlot(gameFilename, g_Config.iStateUndoLastSaveSlot);
	}

	bool HasScreenshotInSlot(const Path &gameFilename, int slot)
	{
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, SCREENSHOT_EXTENSION);
		return File::Exists(fn);
	}

	bool HasUndoLoad(const Path &gameFilename)
	{
		Path fn = GetSysDirectory(DIRECTORY_SAVESTATE) / LOAD_UNDO_NAME;
		return File::Exists(fn) && g_Config.sStateLoadUndoGame == GenerateFullDiscId(gameFilename);
	}

	bool operator < (const tm &t1, const tm &t2) {
		if (t1.tm_year < t2.tm_year) return true;
		if (t1.tm_year > t2.tm_year) return false;
		if (t1.tm_mon < t2.tm_mon) return true;
		if (t1.tm_mon > t2.tm_mon) return false;
		if (t1.tm_mday < t2.tm_mday) return true;
		if (t1.tm_mday > t2.tm_mday) return false;
		if (t1.tm_hour < t2.tm_hour) return true;
		if (t1.tm_hour > t2.tm_hour) return false;
		if (t1.tm_min < t2.tm_min) return true;
		if (t1.tm_min > t2.tm_min) return false;
		if (t1.tm_sec < t2.tm_sec) return true;
		if (t1.tm_sec > t2.tm_sec) return false;
		return false;
	}

	bool operator > (const tm &t1, const tm &t2) {
		if (t1.tm_year > t2.tm_year) return true;
		if (t1.tm_year < t2.tm_year) return false;
		if (t1.tm_mon > t2.tm_mon) return true;
		if (t1.tm_mon < t2.tm_mon) return false;
		if (t1.tm_mday > t2.tm_mday) return true;
		if (t1.tm_mday < t2.tm_mday) return false;
		if (t1.tm_hour > t2.tm_hour) return true;
		if (t1.tm_hour < t2.tm_hour) return false;
		if (t1.tm_min > t2.tm_min) return true;
		if (t1.tm_min < t2.tm_min) return false;
		if (t1.tm_sec > t2.tm_sec) return true;
		if (t1.tm_sec < t2.tm_sec) return false;
		return false;
	}

	bool operator ! (const tm &t1) {
		if (t1.tm_year || t1.tm_mon || t1.tm_mday || t1.tm_hour || t1.tm_min || t1.tm_sec) return false;
		return true;
	}

	int GetNewestSlot(const Path &gameFilename) {
		int newestSlot = -1;
		tm newestDate = {0};
		for (int i = 0; i < NUM_SLOTS; i++) {
			Path fn = GenerateSaveSlotFilename(gameFilename, i, STATE_EXTENSION);
			if (File::Exists(fn)) {
				tm time;
				bool success = File::GetModifTime(fn, time);
				if (success && newestDate < time) {
					newestDate = time;
					newestSlot = i;
				}
			}
		}
		return newestSlot;
	}

	int GetOldestSlot(const Path &gameFilename) {
		int oldestSlot = -1;
		tm oldestDate = {0};
		for (int i = 0; i < NUM_SLOTS; i++) {
			Path fn = GenerateSaveSlotFilename(gameFilename, i, STATE_EXTENSION);
			if (File::Exists(fn)) {
				tm time;
				bool success = File::GetModifTime(fn, time);
				if (success && (!oldestDate || oldestDate > time)) {
					oldestDate = time;
					oldestSlot = i;
				}
			}
		}
		return oldestSlot;
	}

	std::string GetSlotDateAsString(const Path &gameFilename, int slot) {
		Path fn = GenerateSaveSlotFilename(gameFilename, slot, STATE_EXTENSION);
		if (File::Exists(fn)) {
			tm time;
			if (File::GetModifTime(fn, time)) {
				char buf[256];
				// TODO: Use local time format? Americans and some others might not like ISO standard :)
				switch (g_Config.iDateFormat) {
				case PSP_SYSTEMPARAM_DATE_FORMAT_YYYYMMDD:
					strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &time);
					break;
				case PSP_SYSTEMPARAM_DATE_FORMAT_MMDDYYYY:
					strftime(buf, sizeof(buf), "%m-%d-%Y %H:%M:%S", &time);
					break;
				case PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY:
					strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &time);
					break;
				default: // Should never happen
					return "";
				}
				return std::string(buf);
			}
		}
		return "";
	}

	std::vector<Operation> Flush()
	{
		std::lock_guard<std::mutex> guard(mutex);
		std::vector<Operation> copy = pending;
		pending.clear();

		return copy;
	}

	bool HandleLoadFailure()
	{
		// Okay, first, let's give the rewind state a shot - maybe we can at least not reset entirely.
		// Even if this was a rewind, maybe we can still load a previous one.
		CChunkFileReader::Error result;
		do {
			std::string errorString;
			result = rewindStates.Restore(&errorString);
		} while (result == CChunkFileReader::ERROR_BROKEN_STATE);

		if (result == CChunkFileReader::ERROR_NONE) {
			return true;
		}

		// We tried, our only remaining option is to reset the game.
		needsRestart = true;
		// Make sure we don't proceed to run anything yet.
		coreState = CORE_NEXTFRAME;
		return false;
	}

	static inline void CheckRewindState()
	{
		if (gpuStats.numFlips % g_Config.iRewindFlipFrequency != 0)
			return;

		// For fast-forwarding, otherwise they may be useless and too close.
		double now = time_now_d();
		float diff = now - rewindLastTime;
		if (diff < rewindMaxWallFrequency)
			return;

		rewindLastTime = now;
		DEBUG_LOG(BOOT, "Saving rewind state");
		rewindStates.Save();
	}

	bool HasLoadedState() {
		return hasLoadedState;
	}

	bool IsStale() {
		if (saveStateGeneration >= STALE_STATE_USES) {
			return CoreTiming::GetGlobalTimeUs() > STALE_STATE_TIME;
		}
		return false;
	}

	bool IsOldVersion() {
		if (saveStateInitialGitVersion.empty())
			return false;

		Version state(saveStateInitialGitVersion);
		Version gitVer(PPSSPP_GIT_VERSION);
		if (!state.IsValid() || !gitVer.IsValid())
			return false;

		return state < gitVer;
	}

	static Status TriggerLoadWarnings(std::string &callbackMessage) {
		auto sc = GetI18NCategory("Screen");

		if (g_Config.bHideStateWarnings)
			return Status::SUCCESS;

		if (IsStale()) {
			// For anyone wondering why (too long to put on the screen in an osm):
			// Using save states instead of saves simulates many hour play sessions.
			// Sometimes this exposes game bugs that were rarely seen on real devices,
			// because few people played on a real PSP for 10 hours straight.
			callbackMessage = sc->T("Loaded. Save in game, restart, and load for less bugs.");
			return Status::WARNING;
		}
		if (IsOldVersion()) {
			// Save states also preserve bugs from old PPSSPP versions, so warn.
			callbackMessage = sc->T("Loaded. Save in game, restart, and load for less bugs.");
			return Status::WARNING;
		}
		// If the loaded state (saveDataGeneration) is older, the game may prevent saving again.
		// This can happen with newer too, but ignore to/from 0 as a common likely safe case.
		if (saveDataGeneration != lastSaveDataGeneration && saveDataGeneration != 0 && lastSaveDataGeneration != 0) {
			if (saveDataGeneration < lastSaveDataGeneration)
				callbackMessage = sc->T("Loaded. Game may refuse to save over newer savedata.");
			else
				callbackMessage = sc->T("Loaded. Game may refuse to save over different savedata.");
			return Status::WARNING;
		}
		return Status::SUCCESS;
	}

	void Process()
	{
		if (g_Config.iRewindFlipFrequency != 0 && gpuStats.numFlips != 0)
			CheckRewindState();

		if (!needsProcess)
			return;
		needsProcess = false;

		if (!__KernelIsRunning())
		{
			ERROR_LOG(SAVESTATE, "Savestate failure: Unable to load without kernel, this should never happen.");
			return;
		}

		std::vector<Operation> operations = Flush();
		SaveStart state;

		for (size_t i = 0, n = operations.size(); i < n; ++i)
		{
			Operation &op = operations[i];
			CChunkFileReader::Error result;
			Status callbackResult;
			bool tempResult;
			std::string callbackMessage;
			std::string title;

			auto sc = GetI18NCategory("Screen");
			const char *i18nLoadFailure = sc->T("Load savestate failed", "");
			const char *i18nSaveFailure = sc->T("Save State Failed", "");
			if (strlen(i18nLoadFailure) == 0)
				i18nLoadFailure = sc->T("Failed to load state");
			if (strlen(i18nSaveFailure) == 0)
				i18nSaveFailure = sc->T("Failed to save state");

			std::string slot_prefix = op.slot >= 0 ? StringFromFormat("(%d) ", op.slot + 1) : "";
			std::string errorString;

			switch (op.type)
			{
			case SAVESTATE_LOAD:
				INFO_LOG(SAVESTATE, "Loading state from '%s'", op.filename.c_str());
				// Use the state's latest version as a guess for saveStateInitialGitVersion.
				result = CChunkFileReader::Load(op.filename, &saveStateInitialGitVersion, state, &errorString);
				if (result == CChunkFileReader::ERROR_NONE) {
					callbackMessage = op.slot != LOAD_UNDO_SLOT ? sc->T("Loaded State") : sc->T("State load undone");
					callbackResult = TriggerLoadWarnings(callbackMessage);
					hasLoadedState = true;
					Core_ResetException();

					if (!slot_prefix.empty())
						callbackMessage = slot_prefix + callbackMessage;

#ifndef MOBILE_DEVICE
					if (g_Config.bSaveLoadResetsAVdumping) {
						if (g_Config.bDumpFrames) {
							AVIDump::Stop();
							AVIDump::Start(PSP_CoreParameter().renderWidth, PSP_CoreParameter().renderHeight);
						}
						if (g_Config.bDumpAudio) {
							WAVDump::Reset();
						}
					}
#endif
				} else if (result == CChunkFileReader::ERROR_BROKEN_STATE) {
					HandleLoadFailure();
					callbackMessage = std::string(i18nLoadFailure) + ": " + errorString;
					ERROR_LOG(SAVESTATE, "Load state failure: %s", errorString.c_str());
					callbackResult = Status::FAILURE;
				} else {
					callbackMessage = sc->T(errorString.c_str(), i18nLoadFailure);
					callbackResult = Status::FAILURE;
				}
				break;

			case SAVESTATE_SAVE:
				INFO_LOG(SAVESTATE, "Saving state to %s", op.filename.c_str());
				title = g_paramSFO.GetValueString("TITLE");
				if (title.empty()) {
					// Homebrew title
					title = PSP_CoreParameter().fileToStart.ToVisualString();
					std::size_t lslash = title.find_last_of("/");
					title = title.substr(lslash + 1);
				}
				result = CChunkFileReader::Save(op.filename, title, PPSSPP_GIT_VERSION, state);
				if (result == CChunkFileReader::ERROR_NONE) {
					callbackMessage = slot_prefix + sc->T("Saved State");
					callbackResult = Status::SUCCESS;
#ifndef MOBILE_DEVICE
					if (g_Config.bSaveLoadResetsAVdumping) {
						if (g_Config.bDumpFrames) {
							AVIDump::Stop();
							AVIDump::Start(PSP_CoreParameter().renderWidth, PSP_CoreParameter().renderHeight);
						}
						if (g_Config.bDumpAudio) {
							WAVDump::Reset();
						}
					}
#endif
				} else if (result == CChunkFileReader::ERROR_BROKEN_STATE) {
					// TODO: What else might we want to do here? This should be very unusual.
					callbackMessage = i18nSaveFailure;
					ERROR_LOG(SAVESTATE, "Save state failure");
					callbackResult = Status::FAILURE;
				} else {
					callbackMessage = i18nSaveFailure;
					callbackResult = Status::FAILURE;
				}
				break;

			case SAVESTATE_VERIFY:
				tempResult = CChunkFileReader::Verify(state) == CChunkFileReader::ERROR_NONE;
				callbackResult = tempResult ? Status::SUCCESS : Status::FAILURE;
				if (tempResult) {
					INFO_LOG(SAVESTATE, "Verified save state system");
				} else {
					ERROR_LOG(SAVESTATE, "Save state system verification failed");
				}
				break;

			case SAVESTATE_REWIND:
				INFO_LOG(SAVESTATE, "Rewinding to recent savestate snapshot");
				result = rewindStates.Restore(&errorString);
				if (result == CChunkFileReader::ERROR_NONE) {
					callbackMessage = sc->T("Loaded State");
					callbackResult = Status::SUCCESS;
					hasLoadedState = true;
					Core_ResetException();
				} else if (result == CChunkFileReader::ERROR_BROKEN_STATE) {
					// Cripes.  Good news is, we might have more.  Let's try those too, better than a reset.
					if (HandleLoadFailure()) {
						// Well, we did rewind, even if too much...
						callbackMessage = sc->T("Loaded State");
						callbackResult = Status::SUCCESS;
						hasLoadedState = true;
						Core_ResetException();
					} else {
						callbackMessage = std::string(i18nLoadFailure) + ": " + errorString;
						callbackResult = Status::FAILURE;
					}
				} else {
					callbackMessage = std::string(i18nLoadFailure) + ": " + errorString;
					callbackResult = Status::FAILURE;
				}
				break;

			case SAVESTATE_SAVE_SCREENSHOT:
			{
				int maxRes = g_Config.iInternalResolution > 2 ? 2 : -1;
				tempResult = TakeGameScreenshot(op.filename, ScreenshotFormat::JPG, SCREENSHOT_DISPLAY, nullptr, nullptr, maxRes);
				callbackResult = tempResult ? Status::SUCCESS : Status::FAILURE;
				if (!tempResult) {
					ERROR_LOG(SAVESTATE, "Failed to take a screenshot for the savestate! %s", op.filename.c_str());
					if (screenshotFailures++ < SCREENSHOT_FAILURE_RETRIES) {
						// Requeue for next frame.
						SaveScreenshot(op.filename, op.callback, op.cbUserData);
					}
				} else {
					screenshotFailures = 0;
				}
				break;
			}
			default:
				ERROR_LOG(SAVESTATE, "Savestate failure: unknown operation type %d", op.type);
				callbackResult = Status::FAILURE;
				break;
			}

			if (op.callback)
				op.callback(callbackResult, callbackMessage, op.cbUserData);
		}
		if (operations.size()) {
			// Avoid triggering frame skipping due to slowdown
			__DisplaySetWasPaused();
		}
	}

	void NotifySaveData() {
		saveDataGeneration++;
		lastSaveDataGeneration = saveDataGeneration;
	}

	void Cleanup() {
		if (needsRestart) {
			PSP_Shutdown();
			std::string resetError;
			if (!PSP_Init(PSP_CoreParameter(), &resetError)) {
				ERROR_LOG(BOOT, "Error resetting: %s", resetError.c_str());
				// TODO: This probably doesn't clean up well enough.
				Core_Stop();
				return;
			}
			host->BootDone();
			host->UpdateDisassembly();
			needsRestart = false;
		}
	}

	void Init()
	{
		// Make sure there's a directory for save slots
		File::CreateFullPath(GetSysDirectory(DIRECTORY_SAVESTATE));

		std::lock_guard<std::mutex> guard(mutex);
		rewindStates.Clear();

		hasLoadedState = false;
		saveStateGeneration = 0;
		saveDataGeneration = 0;
		lastSaveDataGeneration = 0;
		saveStateInitialGitVersion.clear();
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> guard(mutex);
		rewindStates.Clear();
	}
}
