package org.ppsspp.ppsspp;

import android.Manifest;
import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.UiModeManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.content.res.Configuration;
import android.database.Cursor;
import android.graphics.PixelFormat;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.os.Vibrator;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import androidx.documentfile.provider.DocumentFile;
import android.text.InputType;
import android.util.Log;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;
import java.io.File;
import java.lang.reflect.Field;
import java.util.List;
import java.util.ArrayList;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

@SuppressWarnings("ConstantConditions")
public abstract class NativeActivity extends Activity {
	// Remember to loadLibrary your JNI .so in a static {} block

	// Adjust these as necessary
	private static final String TAG = "PPSSPPNativeActivity";

	// Allows us to skip a lot of initialization on secondary calls to onCreate.
	private static boolean initialized = false;

	// False to use C++ EGL, queried from C++ after NativeApp.init.
	private static boolean javaGL = true;

	// Graphics and audio interfaces for EGL (javaGL = false)
	private NativeSurfaceView mSurfaceView;
	private Surface mSurface;
	private Thread mRenderLoopThread = null;

	// Graphics and audio interfaces for Java EGL (javaGL = true)
	private NativeGLView mGLSurfaceView;
	protected NativeRenderer nativeRenderer;

	private String shortcutParam = "";
	private static String overrideShortcutParam = null;

	public static String runCommand;
	public static String commandParameter;

	// Remember settings for best audio latency
	private int optimalFramesPerBuffer;
	private int optimalSampleRate;

	private boolean sustainedPerfSupported;

	private View navigationCallbackView = null;

	// audioFocusChangeListener to listen to changes in audio state
	private AudioFocusChangeListener audioFocusChangeListener;
	private AudioManager audioManager;
	private PowerManager powerManager;

	private Vibrator vibrator;

	private boolean isXperiaPlay;

	// This is to avoid losing the game/menu state etc when we are just
	// switched-away from or rotated etc.
	private boolean shuttingDown;

	private static final int RESULT_LOAD_IMAGE = 1;
	private static final int RESULT_OPEN_DOCUMENT = 2;
	private static final int RESULT_OPEN_DOCUMENT_TREE = 3;

	// Allow for multiple connected gamepads but just consider them the same for now.
	// Actually this is not entirely true, see the code.
	private ArrayList<InputDeviceState> inputPlayers = new ArrayList<InputDeviceState>();

	private PowerSaveModeReceiver mPowerSaveModeReceiver = null;
	private SizeManager sizeManager = null;
	private static LocationHelper mLocationHelper;
	private static CameraHelper mCameraHelper;

	private static final String[] permissionsForStorage = {
		Manifest.permission.WRITE_EXTERNAL_STORAGE,
	};
	private static final String[] permissionsForLocation = {
		Manifest.permission.ACCESS_FINE_LOCATION,
		Manifest.permission.ACCESS_COARSE_LOCATION,
	};
	private static final String[] permissionsForCamera = {
		Manifest.permission.CAMERA
	};
	private static final String[] permissionsForMicrophone = {
		Manifest.permission.RECORD_AUDIO
	};

	public static final int REQUEST_CODE_STORAGE_PERMISSION = 1;
	public static final int REQUEST_CODE_LOCATION_PERMISSION = 2;
	public static final int REQUEST_CODE_CAMERA_PERMISSION = 3;
	public static final int REQUEST_CODE_MICROPHONE_PERMISSION = 4;

	// Functions for the app activity to override to change behaviour.

	public native void registerCallbacks();
	public native void unregisterCallbacks();

	NativeRenderer getRenderer() {
		return nativeRenderer;
	}

	@TargetApi(Build.VERSION_CODES.JELLY_BEAN_MR1)
	private void detectOptimalAudioSettings() {
		try {
			optimalFramesPerBuffer = Integer.parseInt(this.audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER));
		} catch (NumberFormatException e) {
			// Ignore, if we can't parse it it's bogus and zero is a fine value (means we couldn't detect it).
		}
		try {
			optimalSampleRate = Integer.parseInt(this.audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE));
		} catch (NumberFormatException e) {
			// Ignore, if we can't parse it it's bogus and zero is a fine value (means we couldn't detect it).
		}
	}

	String getApplicationLibraryDir(ApplicationInfo application) {
		String libdir = null;
		try {
			// Starting from Android 2.3, nativeLibraryDir is available:
			Field field = ApplicationInfo.class.getField("nativeLibraryDir");
			libdir = (String) field.get(application);
		} catch (SecurityException e1) {
		} catch (NoSuchFieldException e1) {
		} catch (IllegalArgumentException e) {
		} catch (IllegalAccessException e) {
		}
		if (libdir == null) {
			// Fallback for Android < 2.3:
			libdir = application.dataDir + "/lib";
		}
		return libdir;
	}

	@TargetApi(Build.VERSION_CODES.M)
	boolean askForPermissions(String[] permissions, int requestCode) {
		boolean shouldAsk = false;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
			for (String permission : permissions) {
				if (this.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
					shouldAsk = true;
				}
			}
			if (shouldAsk) {
				this.requestPermissions(permissions, requestCode);
			}
		}
		return shouldAsk;
	}

	@TargetApi(Build.VERSION_CODES.M)
	public void sendInitialGrants() {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
			// Let's start out granted if it was granted already.
			if (this.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED) {
				NativeApp.sendMessage("permission_granted", "storage");
			} else {
				NativeApp.sendMessage("permission_denied", "storage");
			}
		}
	}

	boolean permissionsGranted(String[] permissions, int[] grantResults) {
		for (int i = 0; i < permissions.length; i++) {
			if (grantResults[i] != PackageManager.PERMISSION_GRANTED)
				return false;
		}
		return true;
	}

	@Override
	public void onRequestPermissionsResult(int requestCode, String [] permissions, int[] grantResults) {
		switch (requestCode) {
		case REQUEST_CODE_STORAGE_PERMISSION:
			if (permissionsGranted(permissions, grantResults)) {
				NativeApp.sendMessage("permission_granted", "storage");
			} else {
				NativeApp.sendMessage("permission_denied", "storage");
			}
			break;
		case REQUEST_CODE_LOCATION_PERMISSION:
			if (permissionsGranted(permissions, grantResults)) {
				mLocationHelper.startLocationUpdates();
			}
			break;
		case REQUEST_CODE_CAMERA_PERMISSION:
			if (mCameraHelper != null && permissionsGranted(permissions, grantResults)) {
				mCameraHelper.startCamera();
			}
			break;
		case REQUEST_CODE_MICROPHONE_PERMISSION:
			if (permissionsGranted(permissions, grantResults)) {
				NativeApp.audioRecording_Start();
			}
			break;
		default:
		}
	}

	public void setShortcutParam(String shortcutParam) {
		this.shortcutParam = ((shortcutParam == null) ? "" : shortcutParam);
	}

	// Unofficial hacks to get a list of SD cards that are not the main "external storage".
	private static ArrayList<String> getSdCardPaths(final Context context) {
		// Q is the last version that will support normal file access.
		ArrayList<String> list = null;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT && Build.VERSION.SDK_INT <= Build.VERSION_CODES.Q) {
			Log.i(TAG, "getSdCardPaths: Trying KitKat method");
			list = getSdCardPaths19(context);
		}

		if (list == null) {
			Log.i(TAG, "getSdCardPaths: Attempting fallback");
			// Try another method.
			String removableStoragePath;
			list = new ArrayList<String>();
			File fileList[] = new File("/storage/").listFiles();
			if (fileList != null) {
				for (File file : fileList) {
					if (!file.getAbsolutePath().equalsIgnoreCase(Environment.getExternalStorageDirectory().getAbsolutePath()) && file.isDirectory() && file.canRead()) {
						list.add(file.getAbsolutePath());
					}
				}
			}
		}

		if (list == null) {
			String[] varNames = { "EXTERNAL_SDCARD_STORAGE", "SECONDARY_STORAGE" };
			for (String var : varNames) {
				Log.i(TAG, "getSdCardPaths: Checking env " + var);
				String secStore = System.getenv("SECONDARY_STORAGE");
				if (secStore != null && secStore.length() > 0) {
					list = new ArrayList<String>();
					list.add(secStore);
					break;
				}
			}
		}

		if (list == null) {
			return new ArrayList<String>();
		} else {
			return list;
		}
	}

	@TargetApi(Build.VERSION_CODES.KITKAT)
	private static ArrayList<String> getSdCardPaths19(final Context context)
	{
		final File[] externalCacheDirs = context.getExternalCacheDirs();
		if (externalCacheDirs == null || externalCacheDirs.length==0)
			return null;
		if (externalCacheDirs.length == 1) {
			if (externalCacheDirs[0] == null)
				return null;
			final String storageState = Environment.getStorageState(externalCacheDirs[0]);
			if (!Environment.MEDIA_MOUNTED.equals(storageState))
				return null;
			if (Environment.isExternalStorageEmulated())
				return null;
		}
		final ArrayList<String> result = new ArrayList<>();
		if (externalCacheDirs.length == 1)
			result.add(getRootOfInnerSdCardFolder(externalCacheDirs[0]));
		for (int i = 1; i < externalCacheDirs.length; ++i)
		{
			final File file = externalCacheDirs[i];
			if (file == null)
				continue;
			final String storageState = Environment.getStorageState(file);
			if (Environment.MEDIA_MOUNTED.equals(storageState)) {
				String root = getRootOfInnerSdCardFolder(externalCacheDirs[i]);
				if (root != null) {
					result.add(root);
				}
			}
		}
		if (result.isEmpty())
			return null;
		return result;
	}

	/** Given any file/folder inside an sd card, this will return the path of the sd card */
	private static String getRootOfInnerSdCardFolder(File file)
	{
		if (file == null)
			return null;
		final long totalSpace = file.getTotalSpace();
		if (totalSpace <= 0) {
			return null;
		}
		while (true) {
			final File parentFile = file.getParentFile();
			if (parentFile == null || !parentFile.canRead()) {
				break;
			}
			if (parentFile.getTotalSpace() != totalSpace) {
				break;
			}
			file = parentFile;
		}
		return file.getAbsolutePath();
	}

	public void Initialize() {
		// Initialize audio classes. Do this here since detectOptimalAudioSettings()
		// needs audioManager
		this.audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
		this.audioFocusChangeListener = new AudioFocusChangeListener();

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
			// Get the optimal buffer sz
			detectOptimalAudioSettings();
		}
		powerManager = (PowerManager) getSystemService(Context.POWER_SERVICE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
			if (powerManager != null && powerManager.isSustainedPerformanceModeSupported()) {
				sustainedPerfSupported = true;
				NativeApp.sendMessage("sustained_perf_supported", "1");
			}
		}

		// isLandscape is used to trigger GetAppInfo currently, we
		boolean landscape = NativeApp.isLandscape();
		Log.d(TAG, "Landscape: " + landscape);

		// Get system information
		PackageManager packMgmr = getPackageManager();
		String packageName = getPackageName();

		ApplicationInfo appInfo;
		try {
			appInfo = packMgmr.getApplicationInfo(packageName, 0);
		} catch (NameNotFoundException e) {
			e.printStackTrace();
			throw new RuntimeException("Unable to locate assets, aborting...");
		}

		int deviceType = NativeApp.DEVICE_TYPE_MOBILE;
		UiModeManager uiModeManager = (UiModeManager) getSystemService(UI_MODE_SERVICE);
		switch (uiModeManager.getCurrentModeType()) {
		case Configuration.UI_MODE_TYPE_TELEVISION:
			deviceType = NativeApp.DEVICE_TYPE_TV;
			Log.i(TAG, "Running on an Android TV Device");
			break;
		case Configuration.UI_MODE_TYPE_DESK:
			deviceType = NativeApp.DEVICE_TYPE_DESKTOP;
			Log.i(TAG, "Running on an Android desktop computer (!)");
			break;
		// All other device types are treated the same.
		}

		isXperiaPlay = IsXperiaPlay();

		String libraryDir = getApplicationLibraryDir(appInfo);

		String extStorageState = Environment.getExternalStorageState();
		String extStorageDir = Environment.getExternalStorageDirectory().getAbsolutePath();
		String externalFilesDir = this.getExternalFilesDir(null).getAbsolutePath();

		Log.i(TAG, "Ext storage: " + extStorageState + " " + extStorageDir);
		Log.i(TAG, "Ext files dir: " + externalFilesDir);

		String additionalStorageDirs = "";
		try {
			ArrayList<String> sdCards = getSdCardPaths(this);

			// String.join doesn't exist on old devices (???).
			StringBuilder s = new StringBuilder();
			for (int i = 0; i < sdCards.size(); i++) {
				String sdCard = sdCards.get(i);
				Log.i(TAG, "SD card: " + sdCard);
				s.append(sdCard);
				if (i != sdCards.size() - 1) {
					s.append(":");
				}
			}
			additionalStorageDirs = s.toString();
		}
		catch (Exception e) {
			Log.e(TAG, "Failed to get SD storage dirs: " + e.toString());
		}

		Log.i(TAG, "End of storage paths");

		File filesDir = this.getFilesDir();
		String dataDir = null;
		if (filesDir != null) {
			// Null has been seen in Google Play stacktraces
			dataDir = filesDir.getAbsolutePath();
		}
		String apkFilePath = appInfo.sourceDir;
		String cacheDir = getCacheDir().getAbsolutePath();

		String model = Build.MANUFACTURER + ":" + Build.MODEL;
		String languageRegion = Locale.getDefault().getLanguage() + "_" + Locale.getDefault().getCountry();
		String shortcut = overrideShortcutParam == null ? shortcutParam : overrideShortcutParam;
		overrideShortcutParam = null;

		NativeApp.audioConfig(optimalFramesPerBuffer, optimalSampleRate);
		NativeApp.init(model, deviceType, languageRegion, apkFilePath, dataDir, extStorageDir, externalFilesDir, additionalStorageDirs, libraryDir, cacheDir, shortcut, Build.VERSION.SDK_INT, Build.BOARD);

		// Allow C++ to tell us to use JavaGL or not.
		javaGL = "true".equalsIgnoreCase(NativeApp.queryConfig("androidJavaGL"));

		sendInitialGrants();

		// OK, config should be initialized, we can query for screen rotation.
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.GINGERBREAD) {
			updateScreenRotation("Initialize");
		}

		// Detect OpenGL support.
		// We don't currently use this detection for anything but good to have in the log.
		if (!detectOpenGLES20()) {
			Log.i(TAG, "OpenGL ES 2.0 NOT detected. Things will likely go badly.");
		} else {
			if (detectOpenGLES30()) {
				Log.i(TAG, "OpenGL ES 3.0 detected.");
			} else {
				Log.i(TAG, "OpenGL ES 2.0 detected.");
			}
		}

		vibrator = (Vibrator) getSystemService(VIBRATOR_SERVICE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB) {
			checkForVibrator();
		}

		mLocationHelper = new LocationHelper(this);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB) {
			// android.graphics.SurfaceTexture is not available before version 11.
			mCameraHelper = new CameraHelper(this);
		}
	}

	@TargetApi(Build.VERSION_CODES.N)
	private void updateSustainedPerformanceMode() {
		if (sustainedPerfSupported) {
			// Query the native application on the desired rotation.
			String str = NativeApp.queryConfig("sustainedPerformanceMode");
			try {
				int enable = Integer.parseInt(str);
				getWindow().setSustainedPerformanceMode(enable != 0);
			} catch (NumberFormatException e) {
				Log.e(TAG, "Invalid perf mode: " + str);
			}
		}
	}

	@TargetApi(Build.VERSION_CODES.GINGERBREAD)
	private void updateScreenRotation(String cause) {
		// Query the native application on the desired rotation.
		int rot;
		String rotString = NativeApp.queryConfig("screenRotation");
		try {
			rot = Integer.parseInt(rotString);
		} catch (NumberFormatException e) {
			Log.e(TAG, "Invalid rotation: " + rotString);
			return;
		}
		Log.i(TAG, "Setting requested rotation: " + rot + " ('" + rotString + "') (" + cause + ")");

		switch (rot) {
		case 0:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
			break;
		case 1:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
			break;
		case 2:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
			break;
		case 3:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE);
			break;
		case 4:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT);
			break;
		case 5:
			setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
			break;
		}
	}

	private boolean useImmersive() {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.KITKAT)
			return false;
		String immersive = NativeApp.queryConfig("immersiveMode");
		return immersive.equals("1");
	}

	@SuppressLint("InlinedApi")
	@TargetApi(Build.VERSION_CODES.ICE_CREAM_SANDWICH)
	private void updateSystemUiVisibility() {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
			setupSystemUiCallback();
		}

		// Compute our _desired_ systemUiVisibility
		int flags = View.SYSTEM_UI_FLAG_LOW_PROFILE;
		if (useImmersive()) {
			flags |= View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE;
			flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN;
			flags |= View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN;
		}

		View decorView = getWindow().peekDecorView();
		if (decorView != null) {
			decorView.setSystemUiVisibility(flags);
		} else {
			Log.e(TAG, "updateSystemUiVisibility: decor view not yet created, ignoring for now");
		}
		sizeManager.checkDisplayMeasurements();
	}

	// Need API 11 to check for existence of a vibrator? Zany.
	@TargetApi(Build.VERSION_CODES.HONEYCOMB)
	public void checkForVibrator() {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB) {
			if (!vibrator.hasVibrator()) {
				vibrator = null;
			}
		}
	}

	private final Runnable mEmulationRunner = new Runnable() {
		@Override
		public void run() {
			Log.i(TAG, "Starting the render loop: " + mSurface);
			// Start emulation using the provided Surface.
			if (!runEGLRenderLoop(mSurface)) {
				// Shouldn't happen.
				Log.e(TAG, "Failed to start up OpenGL/Vulkan");
			}
			Log.i(TAG, "Left the render loop: " + mSurface);
		}
	};

	public native boolean runEGLRenderLoop(Surface surface);
	// Tells the render loop thread to exit, so we can restart it.
	public native void exitEGLRenderLoop();

	@Override
	public void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		sizeManager = new SizeManager(this);
		TextRenderer.init(this);
		shuttingDown = false;
		registerCallbacks();

		// This calls NativeApp.setDisplayParameters. Make sure that's done early in order
		// to be able to set defaults when loading config for the first time. Like figuring out
		// whether to start at 1x or 2x.
		sizeManager.updateDisplayMeasurements();

		if (!initialized) {
			Initialize();
			initialized = true;
		}

		if (mPowerSaveModeReceiver == null) {
			mPowerSaveModeReceiver = new PowerSaveModeReceiver(this);
		}

		// OK, config should be initialized, we can query for screen rotation.
		updateScreenRotation("onCreate");
		updateSustainedPerformanceMode();

		setVolumeControlStream(AudioManager.STREAM_MUSIC);

		gainAudioFocus(this.audioManager, this.audioFocusChangeListener);
		NativeApp.audioInit();

		if (javaGL) {
			mGLSurfaceView = new NativeGLView(this);
			nativeRenderer = new NativeRenderer(this);
			mGLSurfaceView.setEGLContextClientVersion(2);
			sizeManager.setSurfaceView(mGLSurfaceView);

			// Setup the GLSurface and ask android for the correct
			// Number of bits for r, g, b, a, depth and stencil components
			// The PSP only has 16-bit Z so that should be enough.
			// Might want to change this for other apps (24-bit might be useful).
			// Actually, we might be able to do without both stencil and depth in
			// the back buffer, but that would kill non-buffered rendering.

			// It appears some gingerbread devices blow up if you use a config chooser at all ???? (Xperia Play)
			//if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB) {

			// On some (especially older devices), things blow up later (EGL_BAD_MATCH) if we don't set
			// the format here, if we specify that we want destination alpha in the config chooser, which we do.
			// http://grokbase.com/t/gg/android-developers/11bj40jm4w/fall-back

			// Needed to avoid banding on Ouya?
			if (Build.MANUFACTURER.equals("OUYA")) {
				mGLSurfaceView.getHolder().setFormat(PixelFormat.RGBX_8888);
				mGLSurfaceView.setEGLConfigChooser(new NativeEGLConfigChooser());
			} else {
				// Tried to mess around with config choosers (NativeEGLConfigChooser) here but fail completely on Xperia Play.

				// Then I tried to require 8888/16/8 but that backfired too, does not work on Mali 450 which is
				// used in popular TVs and boxes like Mi Box. So we'll just get what we get, I guess...

				// if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.ICE_CREAM_SANDWICH && !Build.MANUFACTURER.equals("Amazon")) {
					// mGLSurfaceView.setEGLConfigChooser(8, 8, 8, 8, 16, 8);
				// }
			}
			mGLSurfaceView.setRenderer(nativeRenderer);
			setContentView(mGLSurfaceView);
		} else {
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) {
				updateSystemUiVisibility();
			}

			mSurfaceView = new NativeSurfaceView(NativeActivity.this);
			sizeManager.setSurfaceView(mSurfaceView);
			Log.i(TAG, "setcontentview before");
			setContentView(mSurfaceView);
			Log.i(TAG, "setcontentview after");
			ensureRenderLoop();
		}
	}

	@Override
	public void onWindowFocusChanged(boolean hasFocus) {
		super.onWindowFocusChanged(hasFocus);
		updateSustainedPerformanceMode();
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) {
			updateSystemUiVisibility();
		}
	}

	public void notifySurface(Surface surface) {
		mSurface = surface;
		if (!javaGL) {
			// If we got a surface, this starts the thread. If not, it doesn't.
			if (mSurface == null) {
				joinRenderLoopThread();
			} else {
				ensureRenderLoop();
			}
		}
		updateSustainedPerformanceMode();
	}

	// Invariants: After this, mRenderLoopThread will be set, and the thread will be running.
	protected synchronized void ensureRenderLoop() {
		if (javaGL) {
			Log.e(TAG, "JavaGL - should not get into ensureRenderLoop.");
			return;
		}
		if (mSurface == null) {
			Log.w(TAG, "ensureRenderLoop - not starting thread, needs surface");
			return;
		}

		if (mRenderLoopThread == null) {
			Log.w(TAG, "ensureRenderLoop: Starting thread");
			mRenderLoopThread = new Thread(mEmulationRunner);
			mRenderLoopThread.start();
		}
	}

	// Invariants: After this, mRenderLoopThread will be null, and the thread has exited.
	private synchronized void joinRenderLoopThread() {
		if (javaGL) {
			Log.e(TAG, "JavaGL - should not get into joinRenderLoopThread.");
			return;
		}

		if (mRenderLoopThread != null) {
			// This will wait until the thread has exited.
			Log.i(TAG, "exitEGLRenderLoop");
			exitEGLRenderLoop();
			try {
				Log.i(TAG, "joining render loop thread...");
				mRenderLoopThread.join();
				Log.w(TAG, "Joined render loop thread.");
				mRenderLoopThread = null;
			} catch (InterruptedException e) {
				e.printStackTrace();
			}
		}
	}

	@TargetApi(Build.VERSION_CODES.KITKAT)
	void setupSystemUiCallback() {
		final View decorView = getWindow().peekDecorView();
		if (decorView == null || decorView == navigationCallbackView) {
			return;
		}

		sizeManager.setupSystemUiCallback(decorView);
		navigationCallbackView = decorView;
	}

	@Override
	protected void onStop() {
		super.onStop();
		Log.i(TAG, "onStop - do nothing special");
	}

	@Override
	protected void onDestroy() {
		super.onDestroy();
		Log.i(TAG, "onDestroy");
		if (javaGL) {
			if (nativeRenderer.isRenderingFrame()) {
				Log.i(TAG, "Waiting for renderer to finish.");
				int tries = 200;
				do {
					try {
						Thread.sleep(10);
					} catch (InterruptedException e) {
					}
					tries--;
				} while (nativeRenderer.isRenderingFrame() && tries > 0);
			}
			mGLSurfaceView.onDestroy();
			mGLSurfaceView = null;
		} else {
			mSurfaceView.onDestroy();
			mSurfaceView = null;
		}

		// Probably vain attempt to help the garbage collector...
		audioFocusChangeListener = null;
		audioManager = null;

		sizeManager.setSurfaceView(null);
		if (mPowerSaveModeReceiver != null) {
			mPowerSaveModeReceiver.destroy(this);
			mPowerSaveModeReceiver = null;
		}

		// TODO: Can we ensure that the GL thread has stopped rendering here?
		// I've seen crashes that seem to indicate that sometimes it hasn't...
		NativeApp.audioShutdown();
		if (shuttingDown || isFinishing()) {
			NativeApp.shutdown();
			unregisterCallbacks();
			initialized = false;
		}
		navigationCallbackView = null;
	}

	@Override
	protected void onPause() {
		super.onPause();
		Log.i(TAG, "onPause");
		loseAudioFocus(this.audioManager, this.audioFocusChangeListener);
		NativeApp.pause();
		if (!javaGL) {
			mSurfaceView.onPause();
			Log.i(TAG, "Joining render thread...");
			joinRenderLoopThread();
			Log.i(TAG, "Joined render thread");
		} else {
			if (mGLSurfaceView != null) {
				mGLSurfaceView.onPause();
			} else {
				Log.e(TAG, "mGLSurfaceView really shouldn't be null in onPause");
			}
		}
		if (mCameraHelper != null) {
			mCameraHelper.pause();
		}
		Log.i(TAG, "onPause completed");
	}

	private boolean detectOpenGLES20() {
		ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
		ConfigurationInfo info = am.getDeviceConfigurationInfo();
		return info.reqGlEsVersion >= 0x20000;
	}

	private boolean detectOpenGLES30() {
		ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
		ConfigurationInfo info = am.getDeviceConfigurationInfo();
		return info.reqGlEsVersion >= 0x30000;
	}

	@Override
	protected void onResume() {
		super.onResume();
		updateSustainedPerformanceMode();
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) {
			updateSystemUiVisibility();
		}

		// OK, config should be initialized, we can query for screen rotation.
		if (javaGL || Build.VERSION.SDK_INT >= Build.VERSION_CODES.GINGERBREAD) {
			updateScreenRotation("onResume");
		}

		Log.i(TAG, "onResume");
		if (javaGL) {
			if (mGLSurfaceView != null) {
				mGLSurfaceView.onResume();
			} else {
				Log.e(TAG, "mGLSurfaceView really shouldn't be null in onResume");
			}
		} else {
			if (mSurfaceView != null) {
				mSurfaceView.onResume();
			}
		}
		if (mCameraHelper != null) {
			mCameraHelper.resume();
		}

		gainAudioFocus(this.audioManager, this.audioFocusChangeListener);
		NativeApp.resume();

		if (!javaGL) {
			// Restart the render loop.
			ensureRenderLoop();
		}
	}

	@Override
	public void onAttachedToWindow() {
		Log.i(TAG, "onAttachedToWindow");
		super.onAttachedToWindow();

		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
			setupSystemUiCallback();
		}
	}

	@Override
	public void onConfigurationChanged(Configuration newConfig) {
		Log.i(TAG, "onConfigurationChanged");
		super.onConfigurationChanged(newConfig);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.ICE_CREAM_SANDWICH) {
			updateSystemUiVisibility();
		}
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
			sizeManager.updateDpi((float)newConfig.densityDpi);
		}
	}

	@Override
	public void onMultiWindowModeChanged(boolean isInMultiWindowMode, Configuration newConfig) {
		// onConfigurationChanged not called on multi-window change
		Log.i(TAG, "onMultiWindowModeChanged: isInMultiWindowMode = " + isInMultiWindowMode);
		super.onMultiWindowModeChanged(isInMultiWindowMode, newConfig);
		sizeManager.checkDisplayMeasurements();
	}

	// keep this static so we can call this even if we don't
	// instantiate NativeAudioPlayer
	public static void gainAudioFocus(AudioManager audioManager, AudioFocusChangeListener focusChangeListener) {
		if (audioManager != null) {
			audioManager.requestAudioFocus(focusChangeListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
		}
	}

	// keep this static so we can call this even if we don't
	// instantiate NativeAudioPlayer
	public static void loseAudioFocus(AudioManager audioManager, AudioFocusChangeListener focusChangeListener) {
		if (audioManager != null) {
			audioManager.abandonAudioFocus(focusChangeListener);
		}
	}

	@TargetApi(Build.VERSION_CODES.GINGERBREAD)
	private InputDeviceState getInputDeviceState(InputEvent event) {
		InputDevice device = event.getDevice();
		if (device == null) {
			return null;
		}

		for (InputDeviceState input : inputPlayers) {
			if (input.getDevice() == device) {
				return input;
			}
		}

		// None was found, just add and return it.
		InputDeviceState state = new InputDeviceState(device);
		inputPlayers.add(state);
		Log.i(TAG, "Input player registered: desc = " + getInputDesc(device));
		return state;
	}

	public boolean IsXperiaPlay() {
		return android.os.Build.MODEL.equals("R800a") || android.os.Build.MODEL.equals("R800i") || android.os.Build.MODEL.equals("R800x") || android.os.Build.MODEL.equals("R800at") || android.os.Build.MODEL.equals("SO-01D") || android.os.Build.MODEL.equals("zeus");
	}

	// We grab the keys before onKeyDown/... even see them. This is also better because it lets us
	// distinguish devices.
	@Override
	public boolean dispatchKeyEvent(KeyEvent event) {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB_MR1 && !isXperiaPlay) {
			InputDeviceState state = getInputDeviceState(event);
			if (state == null) {
				return super.dispatchKeyEvent(event);
			}

			// Let's let back and menu through to dispatchKeyEvent.
			boolean passThrough = false;

			int sources = event.getSource();

			// Is this really only for the Xperia Play special handling in OnKeyDown?
			// And if so, can we just handle it here instead?
			switch (event.getKeyCode()) {
			case KeyEvent.KEYCODE_BACK:
				passThrough = true;
				break;
			default:
				break;
			}

			// Don't passthrough back button if from gamepad.
			// XInput device on Android returns source 1281 or 0x501, which equals GAMEPAD | KEYBOARD.
			// Shield Remote returns 769 or 0x301 which equals DPAD | KEYBOARD.

			// Don't disable passthrough if app at top level.
			if (((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
					(sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ||
					(sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD))
			{
				passThrough = false;
			}

			if (!passThrough) {
				switch (event.getAction()) {
				case KeyEvent.ACTION_DOWN:
					if (state.onKeyDown(event)) {
						return true;
					}
					break;

				case KeyEvent.ACTION_UP:
					if (state.onKeyUp(event)) {
						return true;
					}
					break;
				}
			}
		}

		// Let's go through the old path (onKeyUp, onKeyDown).
		return super.dispatchKeyEvent(event);
	}

	@TargetApi(Build.VERSION_CODES.JELLY_BEAN)
	public static String getInputDesc(InputDevice input) {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN) {
			return input.getDescriptor();
		} else {
			List<InputDevice.MotionRange> motions = input.getMotionRanges();
			StringBuilder fakeid = new StringBuilder();
			for (InputDevice.MotionRange range : motions)
				fakeid.append(range.getAxis());
			return fakeid.toString();
		}
	}

	@Override
	@TargetApi(Build.VERSION_CODES.HONEYCOMB_MR1)
	public boolean onGenericMotionEvent(MotionEvent event) {
		// Log.d(TAG, "onGenericMotionEvent: " + event);
		if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != 0) {
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB_MR1) {
				InputDeviceState state = getInputDeviceState(event);
				if (state == null) {
					Log.w(TAG, "Joystick event but failed to get input device state.");
					return super.onGenericMotionEvent(event);
				}
				state.onJoystickMotion(event);
				return true;
			}
		}

		if ((event.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0) {
			switch (event.getAction()) {
			case MotionEvent.ACTION_HOVER_MOVE:
				// process the mouse hover movement...
				return true;
			case MotionEvent.ACTION_SCROLL:
				NativeApp.mouseWheelEvent(event.getX(), event.getY());
				return true;
			}
		}
		return super.onGenericMotionEvent(event);
	}

	@SuppressLint("NewApi")
	@Override
	public boolean onKeyDown(int keyCode, KeyEvent event) {
		// Eat these keys, to avoid accidental exits / other screwups.
		// Maybe there's even more we need to eat on tablets?
		boolean repeat = event.getRepeatCount() > 0;
		switch (keyCode) {
		case KeyEvent.KEYCODE_BACK:
			if (event.isAltPressed()) {
				NativeApp.keyDown(InputDeviceState.deviceId, 1004, repeat); // special custom keycode for the O button on Xperia Play
			} else if (NativeApp.isAtTopLevel()) {
				Log.i(TAG, "IsAtTopLevel returned true.");
				// Pass through the back event.
				return super.onKeyDown(keyCode, event);
			} else {
				NativeApp.keyDown(InputDeviceState.deviceId, keyCode, repeat);
			}
			return true;
		case KeyEvent.KEYCODE_MENU:
		case KeyEvent.KEYCODE_SEARCH:
			NativeApp.keyDown(InputDeviceState.deviceId, keyCode, repeat);
			return true;

		case KeyEvent.KEYCODE_DPAD_UP:
		case KeyEvent.KEYCODE_DPAD_DOWN:
		case KeyEvent.KEYCODE_DPAD_LEFT:
		case KeyEvent.KEYCODE_DPAD_RIGHT:
			// Joysticks are supported in Honeycomb MR1 and later via the onGenericMotionEvent method.
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB_MR1 && event.getSource() == InputDevice.SOURCE_JOYSTICK) {
				// Pass through / ignore
				return super.onKeyDown(keyCode, event);
			}
			// Fall through
		default:
			// send the rest of the keys through.
			// TODO: get rid of the three special cases above by adjusting the native side of the code.
			// Log.d(TAG, "Key down: " + keyCode + ", KeyEvent: " + event);
			return NativeApp.keyDown(InputDeviceState.deviceId, keyCode, repeat);
		}
	}

	@SuppressLint("NewApi")
	@Override
	public boolean onKeyUp(int keyCode, KeyEvent event) {
		switch (keyCode) {
		case KeyEvent.KEYCODE_BACK:
			if (event.isAltPressed()) {
				NativeApp.keyUp(0, 1004); // special custom keycode
			} else if (NativeApp.isAtTopLevel()) {
				Log.i(TAG, "IsAtTopLevel returned true.");
				return super.onKeyUp(keyCode, event);
			} else {
				NativeApp.keyUp(0, keyCode);
			}
			return true;
		case KeyEvent.KEYCODE_MENU:
		case KeyEvent.KEYCODE_SEARCH:
			// Search probably should also be ignored. We send it to the app.
			NativeApp.keyUp(0, keyCode);
			return true;

		case KeyEvent.KEYCODE_DPAD_UP:
		case KeyEvent.KEYCODE_DPAD_DOWN:
		case KeyEvent.KEYCODE_DPAD_LEFT:
		case KeyEvent.KEYCODE_DPAD_RIGHT:
			// Joysticks are supported in Honeycomb MR1 and later via the onGenericMotionEvent method.
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB_MR1 && event.getSource() == InputDevice.SOURCE_JOYSTICK) {
				return super.onKeyUp(keyCode, event);
			}
			// Fall through
		default:
			// send the rest of the keys through.
			// Log.d(TAG, "Key down: " + keyCode + ", KeyEvent: " + event);
			return NativeApp.keyUp(0, keyCode);
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);
		if (resultCode != RESULT_OK || data == null) {
			return;
		}
		if (requestCode == RESULT_LOAD_IMAGE) {
			Uri selectedImage = data.getData();
			if (selectedImage != null) {
				String[] filePathColumn = {MediaStore.Images.Media.DATA};
				Cursor cursor = getContentResolver().query(selectedImage, filePathColumn, null, null, null);
				cursor.moveToFirst();
				int columnIndex = cursor.getColumnIndex(filePathColumn[0]);
				String picturePath = cursor.getString(columnIndex);
				cursor.close();
				NativeApp.sendMessage("bgImage_updated", picturePath);
			}
		} else if (requestCode == RESULT_OPEN_DOCUMENT) {
			Uri selectedFile = data.getData();
			if (selectedFile != null) {
				// Grab permanent permission so we can show it in recents list etc.
				if (Build.VERSION.SDK_INT >= 19) {
					getContentResolver().takePersistableUriPermission(selectedFile, Intent.FLAG_GRANT_READ_URI_PERMISSION);
				}
				Log.i(TAG, "Browse file finished:" + selectedFile.toString());
				NativeApp.sendMessage("browse_fileSelect", selectedFile.toString());
			}
		} else if (requestCode == RESULT_OPEN_DOCUMENT_TREE) {
			Uri selectedDirectoryUri = data.getData();
			if (selectedDirectoryUri != null) {
				String path = selectedDirectoryUri.toString();
				Log.i(TAG, "Browse folder finished: " + path);
				getContentResolver().takePersistableUriPermission(selectedDirectoryUri, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
				DocumentFile documentFile = DocumentFile.fromTreeUri(this, selectedDirectoryUri);
				Log.i(TAG, "Document name: " + documentFile.getUri());
				/*
				// Old debug log
				DocumentFile[] children = documentFile.listFiles();
				for (DocumentFile child : children) {
					Log.i(TAG, "Child: " + child.getUri() + " " + child.getName());
				}
				*/
				NativeApp.sendMessage("browse_folderSelect", documentFile.getUri().toString());
			}
		}
	}

	@TargetApi(Build.VERSION_CODES.HONEYCOMB)
	private AlertDialog.Builder createDialogBuilderWithTheme() {
		return new AlertDialog.Builder(this, AlertDialog.THEME_HOLO_DARK);
	}

	@TargetApi(Build.VERSION_CODES.ICE_CREAM_SANDWICH)
	private AlertDialog.Builder createDialogBuilderWithDeviceTheme() {
		return new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK);
	}

	@TargetApi(Build.VERSION_CODES.JELLY_BEAN_MR1)
	private AlertDialog.Builder createDialogBuilderWithDeviceThemeAndUiVisibility() {
		AlertDialog.Builder bld = new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK);
		bld.setOnDismissListener(new DialogInterface.OnDismissListener() {
			@Override
			public void onDismiss(DialogInterface dialog) {
				updateSystemUiVisibility();
			}
		});
		return bld;
	}

	@TargetApi(Build.VERSION_CODES.M)
	private AlertDialog.Builder createDialogBuilderNew() {
		AlertDialog.Builder bld = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
		bld.setOnDismissListener(new DialogInterface.OnDismissListener() {
			@Override
			public void onDismiss(DialogInterface dialog) {
				updateSystemUiVisibility();
			}
		});
		return bld;
	}

	// The return value is sent to C++ via seqID.
	public void inputBox(final String seqID, final String title, String defaultText, String defaultAction) {
		final FrameLayout fl = new FrameLayout(this);
		final EditText input = new EditText(this);
		input.setGravity(Gravity.CENTER);

		FrameLayout.LayoutParams editBoxLayout = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
		editBoxLayout.setMargins(2, 20, 2, 20);
		fl.addView(input, editBoxLayout);

		input.setInputType(InputType.TYPE_CLASS_TEXT);
		input.setText(defaultText);
		input.selectAll();

		// Lovely!
		AlertDialog.Builder bld;
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.HONEYCOMB)
			bld = new AlertDialog.Builder(this);
		else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.ICE_CREAM_SANDWICH)
			bld = createDialogBuilderWithTheme();
		else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.JELLY_BEAN_MR1)
			bld = createDialogBuilderWithDeviceTheme();
		else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M)
			bld = createDialogBuilderWithDeviceThemeAndUiVisibility();
		else
			bld = createDialogBuilderNew();

		AlertDialog.Builder builder = bld
			.setView(fl)
			.setTitle(title)
			.setPositiveButton(defaultAction, new DialogInterface.OnClickListener() {
				@Override
				public void onClick(DialogInterface d, int which) {
					NativeApp.sendInputBox(seqID, true, input.getText().toString());
					d.dismiss();
				}
			})
			.setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
				@Override
				public void onClick(DialogInterface d, int which) {
					NativeApp.sendInputBox(seqID, false, "");
					d.cancel();
				}
			});
		if (Build.VERSION.SDK_INT > Build.VERSION_CODES.JELLY_BEAN_MR1) {
			builder.setOnDismissListener(new DialogInterface.OnDismissListener() {
				@Override
				public void onDismiss(DialogInterface d) {
					NativeApp.sendInputBox(seqID, false, "");
					updateSystemUiVisibility();
				}
			});
		}
		AlertDialog dlg = builder.create();

		dlg.setCancelable(true);
		dlg.show();
	}

	public boolean processCommand(String command, String params) {
		SurfaceView surfView = javaGL ? mGLSurfaceView : mSurfaceView;
		if (command.equals("launchBrowser")) {
			try {
				Intent i = new Intent(Intent.ACTION_VIEW, Uri.parse(params));
				startActivity(i);
				return true;
			} catch (Exception e) {
				// No browser?
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("launchEmail")) {
			try {
				Intent send = new Intent(Intent.ACTION_SENDTO);
				String uriText;
				uriText = "mailto:email@gmail.com" + "?subject=Your app is..." + "&body=great! Or?";
				uriText = uriText.replace(" ", "%20");
				Uri uri = Uri.parse(uriText);
				send.setData(uri);
				startActivity(Intent.createChooser(send, "E-mail the app author!"));
				return true;
			} catch (Exception e) { // For example, android.content.ActivityNotFoundException
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("bgImage_browse")) {
			try {
				Intent i = new Intent(Intent.ACTION_PICK, MediaStore.Images.Media.EXTERNAL_CONTENT_URI);
				startActivityForResult(i, RESULT_LOAD_IMAGE);
				return true;
			} catch (Exception e) { // For example, android.content.ActivityNotFoundException
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("browse_file")) {
			try {
				Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
				intent.addCategory(Intent.CATEGORY_OPENABLE);
				intent.setType("*/*");
				intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
				// Possible alternative approach:
				// String[] mimeTypes = {"application/octet-stream", "/x-iso9660-image"};
				// intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
				startActivityForResult(intent, RESULT_OPEN_DOCUMENT);
				// intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI, pickerInitialUri);
			} catch (Exception e) {
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("browse_folder")) {
			try {
				Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
				intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
				intent.addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
				intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
				intent.putExtra(Intent.EXTRA_LOCAL_ONLY, true);  // Only allow local folders.
				startActivityForResult(intent, RESULT_OPEN_DOCUMENT_TREE);
				return true;
			} catch (Exception e) {
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("sharejpeg")) {
			try {
				Intent share = new Intent(Intent.ACTION_SEND);
				share.setType("image/jpeg");
				share.putExtra(Intent.EXTRA_STREAM, Uri.parse("file://" + params));
				startActivity(Intent.createChooser(share, "Share Picture"));
				return true;
			} catch (Exception e) { // For example, android.content.ActivityNotFoundException
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("sharetext")) {
			try {
				Intent sendIntent = new Intent();
				sendIntent.setType("text/plain");
				sendIntent.putExtra(Intent.EXTRA_TEXT, params);
				sendIntent.setAction(Intent.ACTION_SEND);
				Intent shareIntent = Intent.createChooser(sendIntent, null);
				startActivity(shareIntent);
				return true;
			} catch (Exception e) { // For example, android.content.ActivityNotFoundException
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("showTwitter")) {
			try {
				String twitter_user_name = params;
				try {
					startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("twitter://user?screen_name=" + twitter_user_name)));
				} catch (Exception e) {
					startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://twitter.com/#!/" + twitter_user_name)));
				}
				return true;
			} catch (Exception e) { // For example, android.content.ActivityNotFoundException
				Log.e(TAG, e.toString());
				return false;
			}
		} else if (command.equals("launchMarket")) {
			// Don't need this, can just use launchBrowser with a market:
			// http://stackoverflow.com/questions/3442366/android-link-to-market-from-inside-another-app
			// http://developer.android.com/guide/publishing/publishing.html#marketintent
			return false;
		} else if (command.equals("toast")) {
			Toast toast = Toast.makeText(this, params, Toast.LENGTH_LONG);
			toast.show();
			Log.i(TAG, params);
			return true;
		} else if (command.equals("showKeyboard") && surfView != null) {
			InputMethodManager inputMethodManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
			// No idea what the point of the ApplicationWindowToken is or if it
			// matters where we get it from...
			inputMethodManager.toggleSoftInputFromWindow(surfView.getApplicationWindowToken(), InputMethodManager.SHOW_FORCED, 0);
			return true;
		} else if (command.equals("hideKeyboard") && surfView != null) {
			InputMethodManager inputMethodManager = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
			inputMethodManager.toggleSoftInputFromWindow(surfView.getApplicationWindowToken(), InputMethodManager.SHOW_FORCED, 0);
			return true;
		} else if (command.equals("inputbox")) {
			String title = "Input";
			String defString = "";
			String[] param = params.split(":@:", 3);
			String seqID = param[0];
			if (param.length > 1 && param[1].length() > 0)
				title = param[1];
			if (param.length > 2)
				defString = param[2];
			Log.i(TAG, "Launching inputbox: #" + seqID + " " + title + " " + defString);
			inputBox(seqID, title, defString, "OK");
			return true;
		} else if (command.equals("vibrate")) {
			int milliseconds = -1;
			if (!params.equals("")) {
				try {
					milliseconds = Integer.parseInt(params);
				} catch (NumberFormatException e) {
				}
			}
			// Special parameters to perform standard haptic feedback
			// operations
			// -1 = Standard keyboard press feedback
			// -2 = Virtual key press
			// -3 = Long press feedback
			// Note that these three do not require the VIBRATE Android
			// permission.
			if (surfView != null) {
				switch (milliseconds) {
				case -1:
					surfView.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP, HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING);
					break;
				case -2:
					surfView.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY, HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING);
					break;
				case -3:
					surfView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS, HapticFeedbackConstants.FLAG_IGNORE_GLOBAL_SETTING);
					break;
				default:
					// Requires the vibrate permission, which we don't have, so disabled.
					// vibrator.vibrate(milliseconds);
					break;
				}
			} else {
				Log.e(TAG, "Can't vibrate, no surface view");
			}
			return true;
		} else if (command.equals("finish")) {
			Log.i(TAG, "Setting shuttingDown = true and calling Finish");
			shuttingDown = true;
			finish();
		} else if (command.equals("rotate")) {
			if (javaGL) {
				updateScreenRotation("rotate");
				if (Build.VERSION.SDK_INT < Build.VERSION_CODES.ICE_CREAM_SANDWICH) {
					Log.i(TAG, "Must recreate activity on rotation");
				}
			} else {
				if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.GINGERBREAD) {
					updateScreenRotation("rotate");
				}
			}
		} else if (command.equals("sustainedPerfMode")) {
			updateSustainedPerformanceMode();
		} else if (command.equals("immersive")) {
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
				updateSystemUiVisibility();
			}
		} else if (command.equals("recreate")) {
			recreate();
		} else if (command.equals("graphics_restart")) {
			Log.i(TAG, "graphics_restart");
			if (params != null && !params.equals("")) {
				overrideShortcutParam = params;
			}
			shuttingDown = true;
			recreate();
		} else if (command.equals("ask_permission") && params.equals("storage")) {
			if (askForPermissions(permissionsForStorage, REQUEST_CODE_STORAGE_PERMISSION)) {
				NativeApp.sendMessage("permission_pending", "storage");
			} else {
				NativeApp.sendMessage("permission_granted", "storage");
			}
		} else if (command.equals("gps_command")) {
			if (params.equals("open")) {
				if (!askForPermissions(permissionsForLocation, REQUEST_CODE_LOCATION_PERMISSION)) {
					mLocationHelper.startLocationUpdates();
				}
			} else if (params.equals("close")) {
				mLocationHelper.stopLocationUpdates();
			}
		} else if (command.equals("camera_command")) {
			if (params.startsWith("startVideo")) {
				Pattern pattern = Pattern.compile("startVideo_(\\d+)x(\\d+)");
				Matcher matcher = pattern.matcher(params);
				if (!matcher.matches())
					return false;
				int width = Integer.parseInt(matcher.group(1));
				int height = Integer.parseInt(matcher.group(2));
				mCameraHelper.setCameraSize(width, height);
				if (mCameraHelper != null && !askForPermissions(permissionsForCamera, REQUEST_CODE_CAMERA_PERMISSION)) {
					mCameraHelper.startCamera();
				}
			} else if (mCameraHelper != null && params.equals("stopVideo")) {
				mCameraHelper.stopCamera();
			}
		} else if (command.equals("microphone_command")) {
			if (params.startsWith("startRecording:")) {
				int sampleRate = Integer.parseInt(params.replace("startRecording:", ""));
				NativeApp.audioRecording_SetSampleRate(sampleRate);
				if (!askForPermissions(permissionsForMicrophone, REQUEST_CODE_MICROPHONE_PERMISSION)) {
					NativeApp.audioRecording_Start();
				}
			} else if (params.equals("stopRecording")) {
				NativeApp.audioRecording_Stop();
			}
		} else if (command.equals("uistate")) {
			Window window = this.getWindow();
			if (params.equals("ingame")) {
				// Keep the screen bright - very annoying if it goes dark when tilting away
				window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
				updateSustainedPerformanceMode();
			} else {
				// Only keep the screen bright ingame.
				window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
			}
		}
		return false;
	}

	@SuppressLint("NewApi")
	@Override
	public void recreate() {
		if (android.os.Build.VERSION.SDK_INT >= Build.VERSION_CODES.HONEYCOMB) {
			super.recreate();
		} else {
			startActivity(getIntent());
			finish();
		}
	}
}
