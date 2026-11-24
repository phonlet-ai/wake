// Host check of the ESP32-S3 acoustic engine against the engine's float path.
//
//   compare <model.pwake> <keywords.json> <16 kHz mono WAV>...
//
// Runs every WAV through a full session (frontend, acoustic model, keyword
// scorer, production fire policy at sensitivity 0.5): FloatModel, then the S3
// engine with 16-bit activations at batch 1, 2, 4 and 8 and with 8-bit
// activations at batch 4 (its portable C build gives the same integers as the
// chip). Prints, per S3 mode,
// how far posteriors and keyword scores move and whether detections change.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "kws/runtime.h"
#include "s3_engine.h"
#include "wav.h"

namespace {

struct Run {
  std::vector<int32_t> q31;
  std::vector<int16_t> scores;
  std::vector<uint8_t> fires;
  int64_t frames = 0;
};

bool run(void *model, const std::string &enrollments, int keywords, const std::vector<int16_t> &pcm,
         Run &out) {
  const double sensitivity = 0.5;
  void *session = kws_session_create(model, enrollments.c_str(), &sensitivity, 1, 1);
  if (!session) {
    std::fprintf(stderr, "session: %s\n", kws_error());
    return false;
  }
  const int64_t capacity = (int64_t)pcm.size() / 512 + 1;
  out.q31.assign(capacity * 40, 0);
  out.scores.assign(capacity * keywords, 0);
  out.fires.assign(capacity * keywords, 0);
  std::vector<uint8_t> resets(capacity);
  out.frames = kws_session_push(session, pcm.data(), (int64_t)pcm.size(), 0, capacity, out.q31.data(),
                                resets.data(), out.scores.data(), nullptr, out.fires.data(), nullptr);
  kws_session_destroy(session);
  return out.frames >= 0;
}

int count_keywords(const std::string &json) {
  int n = 0;
  for (size_t at = 0; (at = json.find("\"threshold_loose\"", at)) != std::string::npos; ++at)
    ++n;
  return n;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: compare model.pwake keywords.json audio.wav...\n");
    return 2;
  }
  std::string enrollments;
  if (FILE *f = std::fopen(argv[2], "rb")) {
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
      enrollments.append(buf, n);
    std::fclose(f);
  }
  const int keywords = count_keywords(enrollments);
  void *model = kws_model_load(argv[1], "en");
  if (!model || keywords < 1) {
    std::fprintf(stderr, "model/keywords: %s\n", kws_error());
    return 1;
  }
  std::printf("kernel %s, %d keywords, %d files\n", esp_wake_s3_kernel(), keywords, argc - 3);

  struct Mode {
    int bits, batch;
  };
  for (Mode mode : {Mode{16, 1}, Mode{16, 2}, Mode{16, 4}, Mode{16, 8}, Mode{8, 4}}) {
    const int bits = mode.bits, delay = mode.batch - 1;
    double sum_dlog = 0, max_dp = 0, max_dscore = 0, sum_dscore = 0;
    long frames = 0, argmax_same = 0, score_values = 0, fires_float = 0, fires_s3 = 0,
         fire_frames_differ = 0, sentinel_flips = 0, files_float = 0, files_s3 = 0;
    for (int i = 3; i < argc; ++i) {
      auto wav = kws::detail::load_wav(argv[i]);
      // Half a second of silence so a detection at the very end is not lost to
      // the batch delay.
      wav.pcm.resize(wav.pcm.size() + 8000, 0);
      if (wav.sample_rate != 16000) {
        std::fprintf(stderr, "%s: not 16 kHz\n", argv[i]);
        return 1;
      }
      Run ref, s3;
      esp_wake_s3_disable();
      if (!run(model, enrollments, keywords, wav.pcm, ref))
        return 1;
      esp_wake_s3_enable(bits, mode.batch);
      if (!run(model, enrollments, keywords, wav.pcm, s3))
        return 1;
      esp_wake_s3_disable();
      // S3 frame t + delay is float frame t.
      for (int64_t t = 0; t + delay < s3.frames; ++t) {
        int best_ref = 0, best_s3 = 0;
        for (int c = 0; c < 40; ++c) {
          const double p = ref.q31[t * 40 + c] / 2147483648.0, q = s3.q31[(t + delay) * 40 + c] / 2147483648.0;
          max_dp = std::fmax(max_dp, std::fabs(p - q));
          sum_dlog += std::fabs(std::log(std::fmax(p, 1e-6)) - std::log(std::fmax(q, 1e-6)));
          if (p > ref.q31[t * 40 + best_ref] / 2147483648.0)
            best_ref = c;
          if (q > s3.q31[(t + delay) * 40 + best_s3] / 2147483648.0)
            best_s3 = c;
        }
        argmax_same += best_ref == best_s3;
        for (int k = 0; k < keywords; ++k) {
          const int a = ref.scores[t * keywords + k], b = s3.scores[(t + delay) * keywords + k];
          // -32768 is the scorer's "no score" (far below every threshold): count flips apart.
          if ((a == INT16_MIN) != (b == INT16_MIN)) {
            ++sentinel_flips;
            continue;
          }
          const double d = std::abs(a - b);
          max_dscore = std::fmax(max_dscore, d);
          sum_dscore += d;
          ++score_values;
          fires_float += ref.fires[t * keywords + k];
          fires_s3 += s3.fires[(t + delay) * keywords + k];
          fire_frames_differ += ref.fires[t * keywords + k] != s3.fires[(t + delay) * keywords + k];
        }
      }
      frames += s3.frames - delay;
      long f_ref = 0, f_s3 = 0;
      for (int64_t j = 0; j < ref.frames * keywords; ++j) {
        f_ref += ref.fires[j];
        f_s3 += s3.fires[j];
      }
      files_float += f_ref > 0;
      files_s3 += f_s3 > 0;
    }
    std::printf(
        "s3 %2d-bit, batch %d: %ld frames | posteriors: argmax agrees %.2f%%, mean |dlog p| %.4f, "
        "max |dp| %.4f | keyword scores (int16): mean |d| %.2f, max |d| %.0f, no-score flips %ld | "
        "fires: float %ld, s3 %ld, differing frames %ld | files with a fire: float %ld, s3 %ld\n",
        bits, mode.batch, frames, 100.0 * argmax_same / frames, sum_dlog / (frames * 40.0), max_dp,
        sum_dscore / score_values, max_dscore, sentinel_flips, fires_float, fires_s3,
        fire_frames_differ, files_float, files_s3);
  }
  kws_model_destroy(model);
  return 0;
}
