package org.ppsspp.ppsspp;

import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.system.StructStatVfs;
import android.system.Os;
import android.os.storage.StorageManager;
import android.content.ContentResolver;
import android.database.Cursor;
import android.provider.DocumentsContract;
import android.os.Environment;
import androidx.documentfile.provider.DocumentFile;
import java.util.ArrayList;
import java.util.UUID;
import java.io.File;

public class PpssppActivity extends NativeActivity {
	private static final String TAG = "PpssppActivity";
	// Key used by shortcut.
	public static final String SHORTCUT_EXTRA_KEY = "org.ppsspp.ppsspp.Shortcuts";
	// Key used for debugging.
	public static final String ARGS_EXTRA_KEY = "org.ppsspp.ppsspp.Args";

	private static boolean m_hasUnsupportedABI = false;
	private static boolean m_hasNoNativeBinary = false;

	public static boolean libraryLoaded = false;

	// Matches the enum in AndroidStorage.h.
	private static final int STORAGE_ERROR_SUCCESS = 0;
	private static final int STORAGE_ERROR_UNKNOWN = -1;
	private static final int STORAGE_ERROR_NOT_FOUND = -2;
	private static final int STORAGE_ERROR_DISK_FULL = -3;
	private static final int STORAGE_ERROR_ALREADY_EXISTS = -4;

	@SuppressWarnings("deprecation")
	public static void CheckABIAndLoadLibrary() {
		if (Build.CPU_ABI.equals("armeabi")) {
			m_hasUnsupportedABI = true;
		} else {
			try {
				System.loadLibrary("ppsspp_jni");
				libraryLoaded = true;
			} catch (UnsatisfiedLinkError e) {
				Log.e(TAG, "LoadLibrary failed, UnsatifiedLinkError: " + e.toString());
				m_hasNoNativeBinary = true;
			}
		}
	}

	static {
		CheckABIAndLoadLibrary();
	}

	public PpssppActivity() {
		super();
	}

	@Override
	public void onCreate(Bundle savedInstanceState) {
		if (m_hasUnsupportedABI || m_hasNoNativeBinary) {
			new Thread() {
				@SuppressWarnings("deprecation")
				@Override
				public void run() {
					Looper.prepare();
					AlertDialog.Builder builder = new AlertDialog.Builder(PpssppActivity.this);
					if (m_hasUnsupportedABI) {
						builder.setMessage(Build.CPU_ABI + " target is not supported.").setTitle("Error starting PPSSPP").create().show();
					} else {
						builder.setMessage("The native part of PPSSPP for ABI " + Build.CPU_ABI + " is missing. Try downloading an official build?").setTitle("Error starting PPSSPP").create().show();
					}
					Looper.loop();
				}
			}.start();

			try {
				Thread.sleep(3000);
			} catch (InterruptedException e) {
				e.printStackTrace();
			}

			System.exit(-1);
			return;
		}

		// In case app launched from homescreen shortcut, get shortcut parameter
		// using Intent extra string. Intent extra will be null if launch normal
		// (from app drawer or file explorer).
		Intent intent = getIntent();
		// String action = intent.getAction();
		Uri data = intent.getData();
		if (data != null) {
			String path = intent.getData().getPath();
			Log.i(TAG, "Found Shortcut Parameter in data: " + path);
			super.setShortcutParam("\"" + path.replace("\\", "\\\\").replace("\"", "\\\"") + "\"");
			// Toast.makeText(getApplicationContext(), path, Toast.LENGTH_SHORT).show();
		} else {
			String param = getIntent().getStringExtra(SHORTCUT_EXTRA_KEY);
			String args = getIntent().getStringExtra(ARGS_EXTRA_KEY);
			Log.e(TAG, "Got ACTION_VIEW without a valid uri, trying param");
			if (param != null) {
				Log.i(TAG, "Found Shortcut Parameter in extra-data: " + param);
				super.setShortcutParam("\"" + param.replace("\\", "\\\\").replace("\"", "\\\"") + "\"");
			} else if (args != null) {
				Log.i(TAG, "Found args parameter in extra-data: " + args);
				super.setShortcutParam(args);
			} else {
				Log.e(TAG, "Shortcut missing parameter!");
				super.setShortcutParam("");
			}
		}
		super.onCreate(savedInstanceState);
	}

	// called by the C++ code through JNI. Dispatch anything we can't directly handle
	// on the gfx thread to the UI thread.
	public void postCommand(String command, String parameter) {
		final String cmd = command;
		final String param = parameter;
		runOnUiThread(new Runnable() {
			@Override
			public void run() {
				processCommand(cmd, param);
			}
		});
	}

	public int openContentUri(String uriString, String mode) {
		try {
			Uri uri = Uri.parse(uriString);
			ParcelFileDescriptor filePfd = getContentResolver().openFileDescriptor(uri, mode);
			if (filePfd == null) {
				Log.e(TAG, "Failed to get file descriptor for " + uriString);
				return -1;
			}
			return filePfd.detachFd();  // Take ownership of the fd.
		} catch (Exception e) {
			Log.e(TAG, "openContentUri exception: " + e.toString());
			return -1;
		}
	}

	private static final String[] columns = new String[] {
		DocumentsContract.Document.COLUMN_DISPLAY_NAME,
		DocumentsContract.Document.COLUMN_SIZE,
		DocumentsContract.Document.COLUMN_FLAGS,
		DocumentsContract.Document.COLUMN_MIME_TYPE,  // check for MIME_TYPE_DIR
		DocumentsContract.Document.COLUMN_LAST_MODIFIED
	};

	private String cursorToString(Cursor c) {
		final int flags = c.getInt(2);
		// Filter out any virtual or partial nonsense.
		// There's a bunch of potentially-interesting flags here btw,
		// to figure out how to set access flags better, etc.
		if ((flags & (DocumentsContract.Document.FLAG_PARTIAL | DocumentsContract.Document.FLAG_VIRTUAL_DOCUMENT)) != 0) {
			return null;
		}

		final String mimeType = c.getString(3);
		final boolean isDirectory = mimeType.equals(DocumentsContract.Document.MIME_TYPE_DIR);
		final String documentName = c.getString(0);
		final long size = c.getLong(1);
		final long lastModified = c.getLong(4);

		String str = "F|";
		if (isDirectory) {
			str = "D|";
		}
		return str + size + "|" + documentName + "|" + lastModified;
	}

	// TODO: Maybe add a cheaper version that doesn't extract all the file information?
	// TODO: Replace with a proper query:
	// * https://stackoverflow.com/questions/42186820/documentfile-is-very-slow
	public String[] listContentUriDir(String uriString) {
		Cursor c = null;
		try {
			Uri uri = Uri.parse(uriString);
			final ContentResolver resolver = getContentResolver();
			final Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
					uri, DocumentsContract.getDocumentId(uri));
			final ArrayList<String> listing = new ArrayList<>();
			c = resolver.query(childrenUri, columns, null, null, null);
			while (c.moveToNext()) {
				String str = cursorToString(c);
				if (str != null) {
					listing.add(str);
				}
			}
			// Is ArrayList weird or what?
			String[] strings = new String[listing.size()];
			return listing.toArray(strings);
		}
		catch (IllegalArgumentException e) {
			// Due to sloppy exception handling in resolver.query, we get this wrapping
			// a FileNotFoundException if the directory doesn't exist.
			return new String[]{};
		}
		catch (Exception e) {
			Log.e(TAG, "listContentUriDir exception: " + e.toString());
			return new String[]{};
		} finally {
			if (c != null) {
				c.close();
			}
		}
	}

	public int contentUriCreateDirectory(String rootTreeUri, String dirName) {
		try {
			Uri uri = Uri.parse(rootTreeUri);
			DocumentFile documentFile = DocumentFile.fromTreeUri(this, uri);
			if (documentFile != null) {
				DocumentFile createdDir = documentFile.createDirectory(dirName);
				return createdDir != null ? STORAGE_ERROR_SUCCESS : STORAGE_ERROR_UNKNOWN;
			} else {
				Log.e(TAG, "contentUriCreateDirectory: fromTreeUri returned null");
				return STORAGE_ERROR_UNKNOWN;
			}
		} catch (Exception e) {
			Log.e(TAG, "contentUriCreateDirectory exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	public int contentUriCreateFile(String rootTreeUri, String fileName) {
		try {
			Uri uri = Uri.parse(rootTreeUri);
			DocumentFile documentFile = DocumentFile.fromTreeUri(this, uri);
			if (documentFile != null) {
				// TODO: Check the file extension and choose MIME type appropriately.
				DocumentFile createdFile = documentFile.createFile("application/octet-stream", fileName);
				return createdFile != null ? STORAGE_ERROR_SUCCESS : STORAGE_ERROR_UNKNOWN;
			} else {
				Log.e(TAG, "contentUriCreateFile: fromTreeUri returned null");
				return STORAGE_ERROR_UNKNOWN;
			}
		} catch (Exception e) {
			Log.e(TAG, "contentUriCreateFile exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	public int contentUriRemoveFile(String fileName) {
		try {
			Uri uri = Uri.parse(fileName);
			DocumentFile documentFile = DocumentFile.fromSingleUri(this, uri);
			if (documentFile != null) {
				return documentFile.delete() ? STORAGE_ERROR_SUCCESS : STORAGE_ERROR_UNKNOWN;
			} else {
				return STORAGE_ERROR_UNKNOWN;
			}
		} catch (Exception e) {
			Log.e(TAG, "contentUriRemoveFile exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	// NOTE: The destination is the parent directory! This means that contentUriCopyFile
	// cannot rename things as part of the operation.
	public int contentUriCopyFile(String srcFileUri, String dstParentDirUri) {
		try {
			Uri srcUri = Uri.parse(srcFileUri);
			Uri dstParentUri = Uri.parse(dstParentDirUri);
			return DocumentsContract.copyDocument(getContentResolver(), srcUri, dstParentUri) != null ? STORAGE_ERROR_SUCCESS : STORAGE_ERROR_UNKNOWN;
		} catch (Exception e) {
			Log.e(TAG, "contentUriCopyFile exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	// NOTE: The destination is the parent directory! This means that contentUriCopyFile
	// cannot rename things as part of the operation.
	public int contentUriMoveFile(String srcFileUri, String srcParentDirUri, String dstParentDirUri) {
		try {
			Uri srcUri = Uri.parse(srcFileUri);
			Uri srcParentUri = Uri.parse(srcParentDirUri);
			Uri dstParentUri = Uri.parse(dstParentDirUri);
			return DocumentsContract.moveDocument(getContentResolver(), srcUri, srcParentUri, dstParentUri) != null ? STORAGE_ERROR_SUCCESS : STORAGE_ERROR_UNKNOWN;
		} catch (Exception e) {
			Log.e(TAG, "contentUriMoveFile exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	public int contentUriRenameFileTo(String fileUri, String newName) {
		try {
			Uri uri = Uri.parse(fileUri);
			// Due to a design flaw, we can't use DocumentFile.renameTo().
			// Instead we use the DocumentsContract API directly.
			// See https://stackoverflow.com/questions/37168200/android-5-0-new-sd-card-access-api-documentfile-renameto-unsupportedoperation.
			Uri newUri = DocumentsContract.renameDocument(getContentResolver(), uri, newName);
			return STORAGE_ERROR_SUCCESS;
		} catch (Exception e) {
			// TODO: More detailed exception processing.
			Log.e(TAG, "contentUriRenameFile exception: " + e.toString());
			return STORAGE_ERROR_UNKNOWN;
		}
	}

	private static void closeQuietly(AutoCloseable closeable) {
		if (closeable != null) {
			try {
				closeable.close();
			} catch (RuntimeException rethrown) {
				throw rethrown;
			} catch (Exception ignored) {
			}
		}
	}

	// Probably slightly faster than contentUriGetFileInfo.
	// Smaller difference now than before I changed that one to a query...
	public boolean contentUriFileExists(String fileUri) {
		Cursor c = null;
		try {
			Uri uri = Uri.parse(fileUri);
			c = getContentResolver().query(uri, new String[] { DocumentsContract.Document.COLUMN_DOCUMENT_ID }, null, null, null);
			return c.getCount() > 0;
		} catch (Exception e) {
			// Log.w(TAG, "Failed query: " + e);
			return false;
		} finally {
			closeQuietly(c);
		}
	}

	public String contentUriGetFileInfo(String fileName) {
		Cursor c = null;
		try {
			Uri uri = Uri.parse(fileName);
			final ContentResolver resolver = getContentResolver();
			c = resolver.query(uri, columns, null, null, null);
			if (c.moveToNext()) {
				String str = cursorToString(c);
				return str;
			} else {
				return null;
			}
		} catch (Exception e) {
			Log.e(TAG, "contentUriGetFileInfo exception: " + e.toString());
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
	}

	// The example in Android documentation uses this.getFilesDir for path.
	// There's also a way to beg the OS for more space, which might clear caches, but
	// let's just not bother with that for now.
	public long contentUriGetFreeStorageSpace(String fileName) {
		try {
			Uri uri = Uri.parse(fileName);
			StorageManager storageManager = getApplicationContext().getSystemService(StorageManager.class);

			ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
			if (pfd == null) {
				Log.w(TAG, "Failed to get free storage space from URI: " + fileName);
				return -1;
			}
			StructStatVfs stats = Os.fstatvfs(pfd.getFileDescriptor());
			long freeSpace = stats.f_bavail * stats.f_bsize;
			pfd.close();
			return freeSpace;
		} catch (Exception e) {
			// FileNotFoundException | ErrnoException e
			// Log.getStackTraceString(e)
			Log.e(TAG, "contentUriGetFreeStorageSpace exception: " + e.toString());
			return -1;
		}
	}

	public long filePathGetFreeStorageSpace(String filePath) {
		try {
			StorageManager storageManager = getApplicationContext().getSystemService(StorageManager.class);
			File file = new File(filePath);
			UUID volumeUUID = storageManager.getUuidForPath(file);
			long availableBytes = storageManager.getAllocatableBytes(volumeUUID);
			return availableBytes;
		}  catch (Exception e) {
			Log.e(TAG, "filePathGetFreeStorageSpace exception: " + e.toString());
			return -1;
		}
	}

	public boolean isExternalStoragePreservedLegacy() {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
			// In 29 and later, we can check whether we got preserved storage legacy.
			return Environment.isExternalStorageLegacy();
		} else {
			// In 28 and earlier, we won't call this - we'll still request an exception.
			return false;
		}
	}
}
