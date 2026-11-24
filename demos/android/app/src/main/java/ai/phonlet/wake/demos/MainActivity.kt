package ai.phonlet.wake.demos

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Bundle
import android.util.TypedValue
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import ai.phonlet.wake.Wake
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread

/** Foreground microphone demo: 16 kHz mono AudioRecord feeds Wake; detections appear on screen. */
class MainActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var log: TextView
    private lateinit var button: Button
    private lateinit var phrases: List<String>
    @Volatile private var listening = false
    private var worker: Thread? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        phrases = readPhrases()
        val padding = dp(20)
        val column = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        column.addView(text("Phonlet Wake", 28f, bold = true))
        column.addView(text("Say one of: " + phrases.joinToString(", ") { "“$it”" }, 16f).apply {
            setPadding(0, dp(8), 0, dp(16))
        })
        button = Button(this).apply { setOnClickListener { toggle() } }
        column.addView(button)
        status = text("", 16f).apply { setPadding(0, dp(12), 0, dp(12)) }
        column.addView(status)
        log = text("", 16f).apply { typeface = Typeface.MONOSPACE }
        column.addView(log)
        val scroll = ScrollView(this).apply { addView(column) }
        // Android 15+ draws apps edge to edge: keep the content clear of the system bars.
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left + padding, bars.top + padding, bars.right + padding, bars.bottom + padding)
            insets
        }
        setContentView(scroll)
        showStopped("Ready. Tap Start and say a phrase.")
    }

    private fun readPhrases(): List<String> {
        // A keyword file: {"model": {...}, "keywords": [...]}, as the threshold generator writes it.
        val file = JSONObject(assets.open("keywords.json").bufferedReader().use { it.readText() })
        val array = file.getJSONArray("keywords")
        return List(array.length()) { array.getJSONObject(it).getString("name") }
    }

    private fun toggle() {
        if (listening) {
            listening = false
            return
        }
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.RECORD_AUDIO), 1)
            return
        }
        start()
    }

    override fun onRequestPermissionsResult(code: Int, permissions: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, permissions, results)
        if (code == 1 && results.firstOrNull() == PackageManager.PERMISSION_GRANTED) start()
        else showStopped("Microphone permission denied.")
    }

    private fun start() {
        val model = assets.open("model.pwake").use { it.readBytes() }
        val keywords = assets.open("keywords.json").bufferedReader().use { it.readText() }
        val sensitivities = DoubleArray(phrases.size) { 0.5 } // one per phrase
        val buffer = ShortArray(Wake.frameLength())
        listening = true
        button.text = getString(R.string.stop)
        status.text = getString(R.string.listening)
        worker = thread(name = "wake-demo") {
            var record: AudioRecord? = null
            try {
                Wake.create(model, keywords, *sensitivities).use { wake ->
                    record = AudioRecord(
                        MediaRecorder.AudioSource.VOICE_RECOGNITION, Wake.sampleRate(),
                        AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                        buffer.size * 4).also { it.startRecording() }
                    report("listening at ${Wake.sampleRate()} Hz")
                    while (listening) {
                        val samples = record!!.read(buffer, 0, buffer.size)
                        if (samples <= 0) continue
                        val index = wake.process(buffer, 0, samples)
                        if (index >= 0) report("✓ ${phrases[index]}")
                    }
                    wake.finish()
                }
                runOnUiThread { showStopped("Stopped.") }
            } catch (error: Exception) {
                listening = false
                runOnUiThread { showStopped("Error: ${error.message}") }
            } finally {
                record?.run { stop(); release() }
            }
        }
    }

    private fun report(line: String) {
        android.util.Log.i("wake-demo", line)
        val time = SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(Date())
        runOnUiThread { log.text = "$time  $line\n${log.text}" }
    }

    private fun showStopped(message: String) {
        button.text = getString(R.string.start)
        status.text = message
        android.util.Log.i("wake-demo", message)
    }

    private fun text(value: String, size: Float, bold: Boolean = false) = TextView(this).apply {
        text = value
        setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    override fun onDestroy() {
        listening = false
        worker?.join(2000)
        super.onDestroy()
    }
}
