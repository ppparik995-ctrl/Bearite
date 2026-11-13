package com.bearite.patcher

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.res.AssetManager
import android.content.res.Configuration
import android.content.res.Resources
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.view.Gravity
import android.view.InputEvent
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import dalvik.system.PathClassLoader
import java.io.File
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Proxy

/**
 * Запускает UnityPlayer из установленной игры, не трогая её саму.
 *
 * Идея (как у лаунчера Geode): собираем свой ClassLoader из APK игры (+split'ы),
 * нашего APK и папок с .so обеих сторон. libunity/libil2cpp игры и наша
 * libbearite оказываются в одном linker namespace, а классы игры (UnityPlayer)
 * создаются поверх нашей Activity. Всё, что игра спрашивает у Context
 * (имя пакета, ресурсы, assets, путь к APK), мы подменяем на данные игры,
 * а системе отдаём настоящие значения лаунчера.
 *
 * Поверх игры на паузе рисуется кнопка «Моды», она открывает GameModMenu (панель в стиле игры).
 */
class GameActivity : Activity() {
    /** Путь к base.apk игры: оттуда редактор достаёт геометрию закрытых мешей. */
    var gameApkPaths: String = ""

    private var levelPrompt: LevelPrompt? = null
    private var chat: ChatOverlay? = null
    var mapSync: MapSync? = null
    private var gameCtx: Context? = null
    private var gameLoader: ClassLoader? = null

    /** Объект плеера Unity: View (старые версии) или UnityPlayerForActivityOrService (Unity 6). */
    private var player: Any? = null
    private var gamePkg: String = GAME_PKG

    private var modMenu: GameModMenu? = null
    private var hud: GameHud? = null
    private var modButton: View? = null
    private var touchBtn: View? = null
    /** Класс Native из загрузчика игры: оттуда читаем состояние паузы. */
    private var nativeCls: Class<*>? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        gamePkg = intent.getStringExtra(EXTRA_PKG) ?: GAME_PKG
        BLog.init(this)
        BLog.i("GameActivity: старт, pid=${Process.myPid()}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), ${Build.MANUFACTURER} ${Build.MODEL}")
        // маркер «игра идёт»: если он остался после выхода, процесс убили аварийно
        try { File(filesDir, RUNNING_MARKER).writeText(System.currentTimeMillis().toString()) } catch (_: Throwable) {}
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            BLog.e("необработанное исключение в потоке ${t.name}", e)
            Process.killProcess(Process.myPid())
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        try {
            launchGame()
        } catch (t: Throwable) {
            BLog.e("запуск игры не удался", t)
            showError(t)
        }
    }

    private fun launchGame() {
        val info = packageManager.getApplicationInfo(gamePkg, 0)
        val ctx = createPackageContext(gamePkg, Context.CONTEXT_IGNORE_SECURITY)
        gameCtx = ctx

        if (info.className?.startsWith("com.pairip.") == true) {
            BLog.e("эта сборка игры защищена проверкой лицензии Google Play (${info.className}), запуск через Bearite невозможен")
            throw IllegalStateException(
                "Эта сборка игры (из Google Play) защищена проверкой лицензии: она работает только под " +
                "своим пакетом и закрывается при запуске через Bearite. Нужна сборка игры без этой защиты " +
                "(например, та, что стоит на телефоне, где Bearite уже работает).")
        }

        val abi = Build.SUPPORTED_64_BIT_ABIS.firstOrNull() ?: Build.SUPPORTED_32_BIT_ABIS.first()
        val gameApks = ArrayList<String>()
        gameApks.add(info.sourceDir)
        gameApkPaths = info.sourceDir
        info.splitSourceDirs?.let { gameApks.addAll(it) }
        val ourApk = applicationInfo.sourceDir

        val libPaths = ArrayList<String>()
        info.nativeLibraryDir?.let { libPaths.add(it) }
        gameApks.forEach { libPaths.add("$it!/lib/$abi") }
        applicationInfo.nativeLibraryDir?.let { libPaths.add(it) }
        libPaths.add("$ourApk!/lib/$abi")

        BLog.i("игра: ${info.sourceDir}, splits=${info.splitSourceDirs?.size ?: 0}, abi=$abi")
        BLog.i("libPath: ${libPaths.joinToString(":")}")

        // parent = BootClassLoader: наши классы приложения не просачиваются,
        // Native загрузится вторым экземпляром именно из этого загрузчика
        val loader = PathClassLoader(
            (gameApks + ourApk).joinToString(":"),
            libPaths.joinToString(":"),
            classLoader.parent
        )
        gameLoader = loader

        val cfg = ModManager(this).configFile.absolutePath
        val nat = loader.loadClass("com.bearite.patcher.Native")
        nativeCls = nat
        nat.getField("pauseListener").set(null, Runnable { runOnUiThread { updateModButton() } })
        nat.getMethod("init", String::class.java).invoke(null, cfg)

        // как в стандартном UnityPlayerActivity
        window.takeSurface(null)
        window.setFormat(PixelFormat.RGBX_8888)

        diagnoseGame(info, gameApks)

        // если создание плеера зависнет, через 8 с запишем, где именно стоит главный поток
        val mainThread = Thread.currentThread()
        val playerReady = java.util.concurrent.atomic.AtomicBoolean(false)
        Thread {
            try { Thread.sleep(8000) } catch (_: InterruptedException) { return@Thread }
            if (!playerReady.get()) {
                BLog.e("плеер не создан за 8 с, главный поток стоит здесь:\n" +
                    mainThread.stackTrace.take(40).joinToString("\n") { "    at $it" })
            }
        }.apply { isDaemon = true }.start()
        val p = createPlayer(loader)
        playerReady.set(true)
        player = p
        val view = playerView(p)

        val root = FrameLayout(this)
        root.addView(
            view,
            FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        )
        val menu = GameModMenu(this, ModManager(this))
        modMenu = menu
        val gameHud = GameHud(this, ModManager(this))
        hud = gameHud
        gameHud.onSound = { p, v, l -> sounds.playFile(p, v, l) }
        gameHud.onStopSound = { p -> sounds.stopFile(p) }
        gameHud.isSoundPlaying = { p -> sounds.isPlaying(p) }
        menu.onClosed = { updateModButton() }
        root.addView(gameHud.root, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        val btn = menuButton()
        modButton = btn
        root.addView(btn)
        root.addView(menu.root, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        levelPrompt = LevelPrompt(this).also { it.start() }
        mapSync = MapSync(this).also { it.start() }
        chat = ChatOverlay(this, root).also { it.onRoomChanged = { updateModButton() }; it.start() }
        updateModButton()
        menuPoll.run()
        // иконка-пазл для кнопки «Меню» в паузе игры: мод берёт её из maps/textures/_puzzle.tex
        Thread { try { MapStore(this).saveTextureBytes("_puzzle", resources.openRawResource(R.drawable.ic_puzzle_menu).use { it.readBytes() }) } catch (_: Throwable) {} }.start()
        sounds.start()
        view.requestFocus()
        BLog.i("UnityPlayer создан: ${p.javaClass.name}")
    }

    /** Что за сборка игры: класс Application, фабрика компонентов, нативные библиотеки, сплиты. */
    private fun diagnoseGame(info: ApplicationInfo, apks: List<String>) {
        try {
            BLog.i("игра: Application=${info.className}, factory=${if (Build.VERSION.SDK_INT >= 28) info.appComponentFactory else "-"}, targetSdk=${info.targetSdkVersion}")
            BLog.i("APK: " + apks.joinToString { File(it).name + " " + File(it).length() / 1024 + "K" })
            val dir = info.nativeLibraryDir?.let { File(it) }
            val libs = dir?.listFiles()?.sortedBy { it.name }
            try { val pi = packageManager.getPackageInfo(packageName, 0); BLog.i("лаунчер установлен: " + java.text.SimpleDateFormat("dd.MM HH:mm:ss", java.util.Locale.US).format(java.util.Date(pi.lastUpdateTime))) } catch (_: Throwable) { }
            BLog.i("lib в ${dir?.path}: " + (libs?.joinToString { it.name + " " + it.length() / 1024 + "K" } ?: "нет"))
        } catch (t: Throwable) {
            BLog.e("диагностика игры не удалась", t)
        }
    }

    /**
     * Unity 6: UnityPlayerForActivityOrService(Context, IUnityPlayerLifecycleEvents),
     * у самого UnityPlayer конструктор внутренний. Старые версии: UnityPlayer(Context).
     */
    private fun createPlayer(loader: ClassLoader): Any {
        val events = lifecycleEvents(loader)
        val report = StringBuilder()
        for (name in listOf(
            "com.unity3d.player.UnityPlayerForActivityOrService",
            "com.unity3d.player.UnityPlayer"
        )) {
            val cls = try {
                loader.loadClass(name)
            } catch (_: ClassNotFoundException) {
                report.append("нет класса ").append(name).append('\n')
                continue
            }
            for (c in cls.constructors) {
                val pt = c.parameterTypes
                if (pt.isEmpty() || !Context::class.java.isAssignableFrom(pt[0])) continue
                val args: Array<Any?> = when {
                    pt.size == 1 -> arrayOf(this)
                    pt.size == 2 && pt[1].name.endsWith("IUnityPlayerLifecycleEvents") -> arrayOf(this, events)
                    pt.size == 2 -> arrayOf(this, null)
                    else -> continue
                }
                BLog.i("конструктор плеера: ${cls.name}(${pt.joinToString { it.simpleName }})")
                return c.newInstance(*args)
            }
            report.append(cls.name).append(": ").append(cls.constructors.joinToString { it.toString() }).append('\n')
        }
        throw IllegalStateException("не нашёл подходящий конструктор плеера Unity:\n$report")
    }

    /** Реализация IUnityPlayerLifecycleEvents: выгрузка/выход плеера закрывают игру. */
    private fun lifecycleEvents(loader: ClassLoader): Any? = try {
        val iface = loader.loadClass("com.unity3d.player.IUnityPlayerLifecycleEvents")
        Proxy.newProxyInstance(loader, arrayOf(iface), InvocationHandler { _, method, _ ->
            when (method.name) {
                "onUnityPlayerQuitted" -> runOnUiThread { finish() }
                "onUnityPlayerUnloaded" -> runOnUiThread { moveTaskToBack(true) }
            }
            null
        })
    } catch (_: ClassNotFoundException) {
        null
    }

    private fun playerView(p: Any): View {
        if (p is View) return p
        val m = p.javaClass.methods.firstOrNull { it.name == "getFrameLayout" && it.parameterTypes.isEmpty() }
            ?: throw IllegalStateException("${p.javaClass.name} не View и не имеет getFrameLayout()")
        return m.invoke(p) as View
    }

    private fun menuButton(): View = View(this).apply { visibility = View.GONE }

    /** Прозрачная область поверх КОПИИ родной кнопки (её создаёт нативный мод): ловит нажатие и открывает меню Bearite. */
    private fun menuTouch(): View = View(this).apply {
        visibility = View.GONE
        isClickable = true
        setOnClickListener {
            modMenu?.show()
            hud?.root?.visibility = View.GONE
        }
    }

    /** Состояние копии кнопки: null — нет данных/ошибка (рисуем свою), "wait" — копия создаётся, иначе положение копии. */
    private fun readMenuBtn(): Pair<String, List<Float>> {
        return try {
            val f = File(File(filesDir, "maps"), "_menubtn")
            if (!f.exists()) return "none" to emptyList()
            val parts = f.readText().trim().split('\t')
            val age = System.currentTimeMillis() - f.lastModified()
            when (parts[0]) {
                "ok" -> "ok" to parts.drop(1).mapNotNull { it.toFloatOrNull() }
                "wait" -> (if (age < 3000) "wait" else "none") to emptyList()
                else -> "none" to emptyList()
            }
        } catch (_: Throwable) { "none" to emptyList() }
    }

    /** Ставит прозрачную область точно на копию кнопки: координаты Unity (пиксели, ноль снизу слева) → экран. */
    private fun placeTouch(v: View, pos: List<Float>) {
        val dm = resources.displayMetrics
        val h = dm.heightPixels
        val k = h / 720f
        val w = (272 * k).toInt()
        val bh = (84 * k).toInt()
        // запасное положение — по эталонному скриншоту: правая колонка, над «Достижения»
        var cx = dm.widthPixels - (20 * k) - w / 2f
        var cy = (192 * k) + bh / 2f
        if (pos.size >= 2) {
            val nx = pos[0]
            val ny = h - pos[1]
            if (nx in 0f..dm.widthPixels.toFloat() && ny in 0f..h.toFloat() && Math.abs(nx - cx) < dm.widthPixels * 0.25f) { cx = nx; cy = ny }
        }
        v.layoutParams = FrameLayout.LayoutParams(w, bh, Gravity.TOP or Gravity.START).apply {
            leftMargin = (cx - w / 2f).toInt()
            topMargin = (cy - bh / 2f).toInt()
        }
    }

    /** Кнопка «Меню» в паузе — копия родной кнопки игры (её создаёт нативный мод), своей нарисованной кнопки нет. */
    private fun updateMenuBtn(paused: Boolean) {}

    /** -1 неизвестно, 0 игра идёт, 1 пауза. */
    private fun pauseState(): Int = try {
        nativeCls?.getField("pauseState")?.getInt(null) ?: -1
    } catch (_: Throwable) {
        -1
    }

    private fun gameMenuOpen(): Boolean = try {
        File(File(filesDir, "maps"), "_menu").readText().trim() == "1"
    } catch (_: Throwable) { false }

    private var lastMenu = false
    private var lastPaused = false
    private var lastClick: String? = null
    private var lastAch: String? = null
    private val achStore by lazy { AchievementStore(this) }
    private val menuPoll = object : Runnable {
        override fun run() {
            val m = gameMenuOpen()
            if (m != lastMenu) { lastMenu = m; updateModButton() } else if (lastPaused) updateMenuBtn(true)
            // нажатие на копию кнопки в меню паузы: нативный мод увеличивает счётчик в maps/_menuclick
            val c = try { File(File(filesDir, "maps"), "_menuclick").readText().trim() } catch (_: Throwable) { "" }
            if (lastClick == null) lastClick = c
            else if (c != lastClick) {
                lastClick = c
                modMenu?.show()
                hud?.root?.visibility = View.GONE
            }
            // новые достижения от модов: запоминаем и показываем уведомление
            try { achStore.poll().forEach { hud?.say("★ Достижение: $it", 3500) } } catch (_: Throwable) {}
            // нажатие на «Достижения» в паузе: открываем своё окно со списком
            val a = try { File(File(filesDir, "maps"), "_achclick").readText().trim() } catch (_: Throwable) { "" }
            if (lastAch == null) lastAch = a
            else if (a != lastAch) { lastAch = a; try { AchievementsWindow(this@GameActivity, GameUi(this@GameActivity)).show() } catch (t: Throwable) { BLog.e("окно достижений", t) } }
            android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(this, 150)
        }
    }

    private fun updateModButton() {
        val btn = modButton ?: return
        val st = pauseState()
        // в мультиплеере пауза не останавливает время (st == 0), поэтому кнопка там видна всегда
        // в комнате пауза не останавливает время: там смотрим, открыто ли меню паузы игры (мод пишет maps/_menu)
        val inRoom = chat?.inRoom == true
        val paused = if (inRoom) gameMenuOpen() else st != 0
        lastPaused = paused
        updateMenuBtn(paused)
        if (!paused && modMenu?.isOpen == true) modMenu?.hide()
        // кнопки модов видны, пока игра идёт (и пока паузу не удаётся определить), и скрыты на паузе и в меню
        val showHud = st != 1 && modMenu?.isOpen != true
        hud?.root?.visibility = if (showHud) View.VISIBLE else View.GONE
        if (showHud) hud?.refresh()
    }

    private fun showError(t: Throwable) {
        val tv = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 12f
            setPadding(32, 48, 32, 32)
            text = "Не удалось запустить игру:\n\n" + t.toString() +
                "\n\nПодробности в логе на главном экране Bearite."
        }
        val sv = ScrollView(this)
        sv.setBackgroundColor(Color.BLACK)
        sv.addView(tv)
        setContentView(sv)
    }

    // ---------------------------------------------------------- подмена Context
    // Игре (всё, что не android.*/java.*/наше) отдаём данные игры, системе - свои.

    private fun fromGame(): Boolean {
        val st = Throwable().stackTrace
        if (st.size < 3) return true
        val c = st[2].className
        return !(c.startsWith("android.") || c.startsWith("com.android.") ||
            c.startsWith("java.") || c.startsWith("javax.") || c.startsWith("dalvik.") ||
            c.startsWith("androidx.") || c.startsWith("com.bearite.") ||
            c.startsWith("com.google.android.material.") ||
            c.startsWith("kotlin.") || c.startsWith("libcore.") || c.startsWith("sun."))
    }

    override fun getPackageName(): String =
        if (gameCtx != null && fromGame()) gamePkg else super.getPackageName()

    override fun getResources(): Resources {
        val g = gameCtx
        return if (g != null && fromGame()) g.resources else super.getResources()
    }

    override fun getAssets(): AssetManager {
        val g = gameCtx
        return if (g != null && fromGame()) g.assets else super.getAssets()
    }

    override fun getApplicationInfo(): ApplicationInfo {
        val g = gameCtx
        return if (g != null && fromGame()) g.applicationInfo else super.getApplicationInfo()
    }

    override fun getPackageCodePath(): String {
        val g = gameCtx
        return if (g != null && fromGame()) g.packageCodePath else super.getPackageCodePath()
    }

    override fun getPackageResourcePath(): String {
        val g = gameCtx
        return if (g != null && fromGame()) g.packageResourcePath else super.getPackageResourcePath()
    }

    override fun getClassLoader(): ClassLoader {
        val l = gameLoader
        return if (l != null && fromGame()) l else super.getClassLoader()
    }

    // ------------------------------------------------------- жизненный цикл

    /** Вызывает первый найденный метод плеера из names (имена разные в версиях Unity). */
    private fun callPlayer(names: List<String>, vararg args: Any?) {
        val p = player ?: return
        try {
            val m = p.javaClass.methods.firstOrNull {
                it.name in names && it.parameterTypes.size == args.size
            } ?: return
            m.invoke(p, *args)
        } catch (t: Throwable) {
            BLog.e("UnityPlayer.${names.first()}", t)
        }
    }

    private fun callPlayer(name: String, vararg args: Any?) = callPlayer(listOf(name), *args)

    private fun inject(e: InputEvent): Boolean {
        if (modMenu?.isOpen == true) return false  // пока открыто меню модов, ввод не уходит в игру
        val p = player ?: return false
        return try {
            val m = p.javaClass.methods.firstOrNull { it.name == "injectEvent" && it.parameterTypes.size == 1 }
                ?: return false
            (m.invoke(p, e) as? Boolean) ?: false
        } catch (_: Throwable) {
            false
        }
    }

    override fun onStart() { BLog.i("lifecycle: onStart"); super.onStart(); callPlayer(listOf("onStart", "start")) }
    override fun onStop() { BLog.i("lifecycle: onStop"); callPlayer(listOf("onStop", "stop")); try { sounds.stopAll() } catch (_: Throwable) {}; super.onStop() }
    override fun onResume() { BLog.i("lifecycle: onResume"); super.onResume(); callPlayer("resume") }
    override fun onPause() { BLog.i("lifecycle: onPause"); callPlayer("pause"); super.onPause() }

    override fun onDestroy() {
        try { sounds.stop() } catch (_: Throwable) {}
        BLog.i("GameActivity: нормальный выход")
        try { levelPrompt?.stop() } catch (_: Throwable) {}
        try { chat?.stop() } catch (_: Throwable) {}
        try { File(filesDir, RUNNING_MARKER).delete() } catch (_: Throwable) {}
        val hasDestroy = player?.javaClass?.methods?.any { it.name == "destroy" && it.parameterTypes.isEmpty() } == true
        callPlayer(if (hasDestroy) "destroy" else "quit")
        super.onDestroy()
        // Unity нельзя перезапустить в том же процессе
        Process.killProcess(Process.myPid())
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        callPlayer("newIntent", intent)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        callPlayer("configurationChanged", newConfig)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        BLog.i("lifecycle: фокус окна = $hasFocus")
        callPlayer("windowFocusChanged", hasFocus)
    }

    override fun onLowMemory() {
        super.onLowMemory()
        callPlayer("lowMemory")
    }

    // ввод: как в стандартном UnityPlayerActivity, события отдаём плееру
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_MULTIPLE && player != null) return inject(event)
        return super.dispatchKeyEvent(event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && modMenu?.isOpen == true) {
            modMenu?.hide()
            return true
        }
        return inject(event) || super.onKeyUp(keyCode, event)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && modMenu?.isOpen == true) return true
        return inject(event) || super.onKeyDown(keyCode, event)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean =
        inject(event) || super.onTouchEvent(event)

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        inject(event) || super.onGenericMotionEvent(event)

    private val sounds by lazy { SoundPlayer(this) }
    private var imageCallback: ((android.net.Uri?) -> Unit)? = null

    /** Выбор картинки из галереи (для своих текстур в редакторе карт). */
    fun pickImage(cb: (android.net.Uri?) -> Unit) = pickFile("image/*", cb)

    /** Выбор любого файла (например .zip с пропом). */
    fun pickFile(mime: String, cb: (android.net.Uri?) -> Unit) {
        imageCallback = cb
        try {
            startActivityForResult(
                Intent(Intent.ACTION_GET_CONTENT).apply { type = mime; addCategory(Intent.CATEGORY_OPENABLE) },
                REQ_IMAGE
            )
        } catch (t: Throwable) {
            imageCallback = null
            BLog.e("не открылся выбор картинки", t)
            cb(null)
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == REQ_IMAGE) {
            val cb = imageCallback
            imageCallback = null
            cb?.invoke(if (resultCode == RESULT_OK) data?.data else null)
            return
        }
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
    }

    companion object {
        const val REQ_IMAGE = 4711
        const val EXTRA_PKG = "game_pkg"
        const val RUNNING_MARKER = "game_running"
    }
}
