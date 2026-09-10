package dev.nightglass.companion.premium

import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputFilter
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.addTextChangedListener
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import dev.nightglass.companion.MainActivity
import dev.nightglass.companion.ble.NightglassConnectionService

/** Daily-use control center. Advanced provisioning/recovery retains its existing screen. */
class ControlCenterActivity : AppCompatActivity() {
    private val ink = Color.rgb(241, 245, 244)
    private val muted = Color.rgb(158, 171, 165)
    private val surface = Color.rgb(18, 25, 22)
    private lateinit var root: LinearLayout
    private lateinit var link: TextView
    private lateinit var saveStatus: TextView
    private lateinit var preview: FacePreview
    private lateinit var deck: LinearLayout
    private var draft = PremiumProfile()
    private val handler = Handler(Looper.getMainLooper())
    private val refresh = object : Runnable {
        override fun run() {
            link.text = NightglassConnectionService.lastStatus(this@ControlCenterActivity)?.text ?: "Connect your watch to get started"
            saveStatus.text = PremiumStore.currentStatus()
            handler.postDelayed(this, 1000)
        }
    }
    private val importPack = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) runCatching {
            val text = contentResolver.openInputStream(uri)?.use { input ->
                val bytes = ByteArray(8193)
                var length = 0
                while (length < bytes.size) {
                    val count = input.read(bytes, length, bytes.size - length)
                    if (count < 0) break
                    if (count == 0) break
                    length += count
                }
                require(length <= 8192) { "Face pack is larger than 8 KB" }
                String(bytes, 0, length, Charsets.UTF_8)
            } ?: error("Could not read face pack")
            val imported = PremiumProfile.fromJson(text)
            AlertDialog.Builder(this).setTitle("Preview ${imported.name}?")
                .setMessage("This changes your phone draft only. Review the face, cards and display options, then choose Apply to watch. No credentials or pairing data are imported.")
                .setNegativeButton("Cancel", null).setPositiveButton("Preview") { _, _ ->
                    draft = imported; PremiumStore.saveDraft(this, draft); render()
                }.show()
        }.onFailure { showError(it) }
    }
    private val exportPack = registerForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) runCatching {
            draft.validate()
            contentResolver.openOutputStream(uri)?.use { it.write(draft.json().toByteArray()) }
                ?: error("Could not write backup")
            Toast.makeText(this, "Appearance backup saved", Toast.LENGTH_SHORT).show()
        }.onFailure { showError(it) }
    }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        draft = state?.getString("draft")?.let { runCatching { PremiumProfile.fromJson(it) }.getOrNull() }
            ?: PremiumStore.draft(this)
        window.statusBarColor = Color.BLACK; window.navigationBarColor = Color.BLACK
        render()
    }
    override fun onResume() { super.onResume(); handler.removeCallbacks(refresh); handler.post(refresh) }
    override fun onPause() { handler.removeCallbacks(refresh); super.onPause() }
    override fun onSaveInstanceState(out: Bundle) { out.putString("draft", draft.json()); super.onSaveInstanceState(out) }
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun background(color: Int, radius: Int = 20) = GradientDrawable().apply { setColor(color); cornerRadius = dp(radius).toFloat() }
    private fun heading(parent: LinearLayout, title: String, size: Float = 24f): TextView = TextView(this).also {
        it.text = title; it.textSize = size; it.setTextColor(ink); it.typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        it.setPadding(0, dp(6), 0, dp(12)); parent.addView(it)
    }
    private fun body(parent: LinearLayout, text: String): TextView = TextView(this).also {
        it.text = text; it.textSize = 15f; it.setTextColor(muted); it.setLineSpacing(dp(3).toFloat(), 1f)
        it.setPadding(0, 0, 0, dp(12)); parent.addView(it)
    }
    private fun card(title: String, note: String = "", content: (LinearLayout) -> Unit) {
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; background = background(surface)
            setPadding(dp(20), dp(18), dp(20), dp(18))
        }
        root.addView(layout, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(16) })
        heading(layout, title, 22f); if (note.isNotBlank()) body(layout, note); content(layout)
    }
    private fun button(parent: LinearLayout, text: String, primary: Boolean = false, action: () -> Unit): Button = Button(this).also {
        it.text = text; it.isAllCaps = false; it.textSize = 16f; it.minHeight = dp(52)
        it.setTextColor(if (primary) Color.BLACK else ink)
        it.backgroundTintList = ColorStateList.valueOf(if (primary) Color.rgb(168, 255, 50) else Color.rgb(35, 47, 40))
        parent.addView(it, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
        it.setOnClickListener { action() }
    }
    private fun choose(parent: LinearLayout, title: String, options: List<String>, selected: Int, changed: (Int) -> Unit) {
        body(parent, title)
        val spinner = Spinner(this).apply {
            adapter = ArrayAdapter(this@ControlCenterActivity, android.R.layout.simple_spinner_item, options)
                .also { it.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item) }
            minimumHeight = dp(48); contentDescription = title; setSelection(selected)
        }
        parent.addView(spinner, LinearLayout.LayoutParams(-1, dp(56)))
        spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                (view as? TextView)?.setTextColor(ink)
                changed(position)
            }
            override fun onNothingSelected(parent: AdapterView<*>?) = Unit
        }
    }
    private fun edit(update: PremiumProfile) {
        draft = update
        runCatching { PremiumStore.saveDraft(this, draft) }
        if (::preview.isInitialized) preview.invalidate()
    }
    private fun toggle(parent: LinearLayout, text: String, checked: Boolean, action: (Boolean) -> Unit) {
        val control = androidx.appcompat.widget.SwitchCompat(this).apply {
            this.text = text; textSize = 17f; setTextColor(ink); minHeight = dp(58); isChecked = checked
        }
        parent.addView(control); control.setOnCheckedChangeListener { _, on -> action(on) }
    }
    private fun render() {
        root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(30), dp(20), dp(32)); setBackgroundColor(Color.BLACK)
        }
        val scroll = ScrollView(this).apply { isFillViewport = true; addView(root) }
        setContentView(scroll)
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            root.setPadding(dp(20) + bars.left, dp(24) + bars.top, dp(20) + bars.right, dp(28) + bars.bottom)
            insets
        }
        ViewCompat.requestApplyInsets(scroll)
        heading(root, "Nightglass", 34f)
        body(root, "Your watch. Your rhythm.")
        card("Watch connection") { box ->
            link = body(box, NightglassConnectionService.lastStatus(this)?.text ?: "Ready when you are")
            button(box, "Connections & advanced setup") { startActivity(Intent(this, MainActivity::class.java)) }
        }
        card("Make it yours", "Preview uses sample values. Actual data comes from your watch and phone.") { box ->
            preview = FacePreview(); box.addView(preview, LinearLayout.LayoutParams(-1, dp(330)))
            val name = EditText(this).apply {
                setText(draft.name); hint = "Face name"; setTextColor(ink); setHintTextColor(muted)
                filters = arrayOf(InputFilter.LengthFilter(24)); isSingleLine = true; contentDescription = "Face name"
            }
            box.addView(name); name.addTextChangedListener { edit(draft.copy(name = it?.toString().orEmpty())) }
            choose(box, "Face layout", PremiumProfile.faceNames, draft.face) { edit(draft.copy(face = it)) }
            val colors = listOf("Lime" to 0xa8ff32, "Glacier" to 0x63dde4, "Lilac" to 0xc6abff,
                "Amber" to 0xffbf69, "Rose" to 0xffb0ce, "Pearl" to 0xf3f7f4)
            val colorOptions = if (colors.none { it.second == draft.accent }) colors + ("Imported color" to draft.accent) else colors
            choose(box, "Accent", colorOptions.map { it.first }, colorOptions.indexOfFirst { it.second == draft.accent }.coerceAtLeast(0)) {
                edit(draft.copy(accent = colorOptions[it].second))
            }
            body(box, "Complications appear on the Personal face. Large Text also uses this layout.")
            repeat(3) { slot ->
                choose(box, "Complication ${slot + 1}", PremiumProfile.complicationNames, draft.complications[slot]) { value ->
                    edit(draft.copy(complications = draft.complications.toMutableList().also { it[slot] = value }))
                }
            }
        }
        card("Context Deck", "Choose the cards you see and their order.") { box ->
            deck = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }; box.addView(deck); renderDeck()
        }
        card("Display & comfort") { box ->
            toggle(box, "Always On display", draft.alwaysOn) { edit(draft.copy(alwaysOn = it)) }
            body(box, "A dim, moving clock on black. Uses more battery; pauses at 15% or if battery data is unavailable.")
            toggle(box, "Large Text", draft.largeText) { edit(draft.copy(largeText = it)) }
            body(box, "Larger reading text and a spacious Personal watch face.")
            toggle(box, "Reduce Motion", draft.reduceMotion) { edit(draft.copy(reduceMotion = it)) }
            saveStatus = body(box, PremiumStore.currentStatus())
            button(box, "Apply to watch", true) {
                runCatching { PremiumStore.apply(this, draft.validate()) }.onFailure(::showError)
                saveStatus.text = PremiumStore.currentStatus()
            }
            button(box, "Load last confirmed watch settings") {
                val confirmed = PremiumStore.confirmed(this)
                if (confirmed == null) Toast.makeText(this, "No watch snapshot yet", Toast.LENGTH_SHORT).show()
                else AlertDialog.Builder(this).setTitle("Replace your phone draft?")
                    .setMessage("Loads the last confirmed appearance snapshot. Reconnect first if settings changed on the watch.")
                    .setNegativeButton("Cancel", null).setPositiveButton("Load") { _, _ -> edit(confirmed); render() }.show()
            }
        }
        card("Face packs & backup", "Portable JSON stores appearance, complications, card order and display options only. Never Wi-Fi, pairing keys or OpenClaw credentials.") { box ->
            button(box, "Import face pack / restore appearance") { importPack.launch(arrayOf("application/json", "text/plain")) }
            button(box, "Export appearance backup") {
                runCatching { draft.validate(); exportPack.launch("nightglass-appearance.json") }.onFailure(::showError)
            }
        }
        card("Apps on your wrist", "Start playback on your phone, then open Artwork / Queue / Output on the watch. Conversation + Actions shows history and actions shared by each app.") { box ->
            button(box, "Phone volume settings") {
                val intent = if (android.os.Build.VERSION.SDK_INT >= 29) Intent(Settings.Panel.ACTION_VOLUME)
                    else Intent(Settings.ACTION_SOUND_SETTINGS)
                runCatching { startActivity(intent) }.onFailure(::showError)
            }
            button(box, "Notification access") { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
            button(box, "OpenClaw & voice setup") { startActivity(Intent(this, MainActivity::class.java)) }
            body(box, "Queue selection and reactions appear only when the provider exposes them. Full Discord history and attachment viewing stay in Discord.")
        }
    }
    private fun renderDeck() {
        deck.removeAllViews()
        draft.deckOrder.forEachIndexed { position, id ->
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            val enabled = CheckBox(this).apply {
                text = PremiumProfile.cardNames[id]; setTextColor(ink); textSize = 16f; minHeight = dp(52)
                isChecked = draft.deckMask and (1 shl id) != 0
            }
            row.addView(enabled, LinearLayout.LayoutParams(0, -2, 1f))
            enabled.setOnCheckedChangeListener { control, checked ->
                val mask = if (checked) draft.deckMask or (1 shl id) else draft.deckMask and (1 shl id).inv()
                if (mask == 0) { control.isChecked = true; Toast.makeText(this, "Keep one card enabled", Toast.LENGTH_SHORT).show() }
                else edit(draft.copy(deckMask = mask))
            }
            listOf(-1 to "Up", 1 to "Down").forEach { (direction, label) ->
                row.addView(Button(this).apply {
                    text = label; isAllCaps = false; textSize = 12f; setTextColor(ink)
                    contentDescription = "Move ${PremiumProfile.cardNames[id]} $label"
                    isEnabled = position + direction in 0..4
                    setOnClickListener {
                        val order = draft.deckOrder.toMutableList()
                        val next = position + direction; order[position] = order[next]; order[next] = id
                        edit(draft.copy(deckOrder = order)); renderDeck()
                    }
                }, LinearLayout.LayoutParams(dp(66), dp(52)))
            }
            deck.addView(row)
        }
    }
    private fun showError(error: Throwable) {
        AlertDialog.Builder(this).setTitle("Could not complete that")
            .setMessage(error.message ?: "Please try again.").setPositiveButton("OK", null).show()
    }
    private inner class FacePreview : View(this@ControlCenterActivity) {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        init { contentDescription = "Sample watch-face preview"; importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO }
        override fun onDraw(canvas: Canvas) {
            super.onDraw(canvas)
            val scale = minOf(width / 410f, height / 502f) * .96f
            canvas.save(); canvas.translate((width - 410 * scale) / 2, (height - 502 * scale) / 2); canvas.scale(scale, scale)
            paint.color = Color.rgb(49, 61, 54); canvas.drawRoundRect(0f, 0f, 410f, 502f, 68f, 68f, paint)
            paint.color = Color.BLACK; canvas.drawRoundRect(5f, 5f, 405f, 497f, 64f, 64f, paint)
            fun text(value: String, x: Float, y: Float, size: Float, color: Int = ink) {
                paint.color = color; paint.textSize = size; paint.typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
                canvas.drawText(value, x, y, paint)
            }
            val accent = draft.accent or (255 shl 24)
            if (draft.face == 2 || draft.largeText) {
                text(draft.name.take(22), 32f, 62f, 20f, accent); text("10:09", 32f, 128f, 48f)
                text("SATURDAY, SEPT 5", 32f, 162f, 18f, muted)
                val values = listOf("82%", "6,420", "72 F", "05:00", "07:30", "3 new", "2.8 mi", "Connected")
                draft.complications.forEachIndexed { i, field ->
                    val y = 184f + i * 78f; paint.color = surface; canvas.drawRoundRect(28f, y, 382f, y + 68, 14f, 14f, paint)
                    text(PremiumProfile.complicationNames[field].uppercase(), 42f, y + 22, 14f, muted)
                    text(values[field], 42f, y + 53, 26f, accent)
                }
                text("CONTEXT                 APPS", 42f, 461f, 20f, accent)
            } else {
                text(PremiumProfile.faceNames[draft.face].uppercase(), 42f, 62f, 22f, accent)
                if (draft.face == 1) {
                    paint.color = Color.rgb(35, 53, 35); paint.strokeWidth = 2f
                    for (x in 40..370 step 55) canvas.drawLine(x.toFloat(), 85f, x.toFloat(), 414f, paint)
                    for (y in 100..420 step 55) canvas.drawLine(30f, y.toFloat(), 380f, y.toFloat(), paint)
                }
                text("82%", 164f, 127f, 24f, accent); text("6,420 steps", 38f, 193f, 20f)
                text("72 F", 288f, 193f, 20f); text("10:09", 62f, 302f, 68f, accent)
                text("SATURDAY     SEP 5", 76f, 362f, 20f, muted); text("APPS", 172f, 462f, 18f, accent)
            }
            canvas.restore()
        }
    }
}
