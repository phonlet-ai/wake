#include "json.h"
#include "keyword_scorer.h"
#include "kws/runtime.h"
#include "kws/runtime.hpp"
#include "wav.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
const char *help = R"HELP(kws-engine — native streaming keyword inference

Usage:
  kws-engine COMMAND [OPTIONS]
  kws-engine --help
  kws-engine --list-languages
  kws-engine --list-scorers

Commands:
  mel          WAV -> lossless int16 mel frames; no model required
  detect       WAV -> keyword detections, optionally save native arrays

Options:
  --lang CODE             Language preset for the entire operation (default: en)
  --input PATH            Prepared mono 16 kHz PCM16 WAV
  --model PATH            Native model container; required for detect
  --keyword PATH          Enrollment JSON; repeat for multiple keywords
  --sensitivities LIST     Comma-separated values in [0,1] (default: 0,.25,.5,.75,1)
  --mode MODE             independent (default) or production_arbitrated
  --output PATH           Required binary output for mel
  --scores PATH           Save reported int16 scores during detect
  --finalize POLICY       mel only: training_nearest_hop (default) or complete_hops
  --scorer NAME           detect: stage-2 scorer for every keyword, overriding
                          the enrollment's "scorer" field (default: production)
  --scorer-options STR    detect: that scorer's options, e.g. maxdur=8,gop=0.1
  --model PATH accepts a quantized (int8) or float (any width, dense or low
                          rank, fp32/fp16/int8 weights) container, or a cifg_lstm
                          training checkpoint (u*.ckpt) directly, chosen by content
  --expect-keyword        One-keyword positive clip: report hit/miss at each sensitivity
  --help, -h              Show this help without loading a model
  --list-languages        Show the bundled presets and their output dimensions
  --list-scorers          Show the keyword scorers built into this library

Inputs and outputs:
  WAV input must be mono, 16 kHz, PCM16. No implicit resampling.
  Model and enrollment dimensions must match the selected language preset.
  Each command starts a fresh stream. Inference drops incomplete 512-sample EOF
  frames; it never adds silence. Detection times are frame-end seconds.
  Binary arrays are little-endian, row-major, with a PATH.json metadata sidecar.
  Mel has [frames, 40] shape. Frontend arithmetic is shared across languages.
  JSON results go to stdout; errors go to stderr. Exit 0 means successful execution,
  including a reported miss; 1 means execution failure, 2 means invalid arguments.
  A hit/miss requires an expected keyword. Without --expect-keyword, results are
  detections only. Timed multi-occurrence matching belongs to the analysis API.

Examples:
  kws-engine mel --input clip.wav --output clip.mel
  kws-engine detect --model en.bin --input clip.wav --keyword keyword.json --expect-keyword
)HELP";

struct Arguments {
  std::string command;
  std::map<std::string, std::string> values;
  std::vector<std::string> keywords;
  bool expect = false;
  std::string get(const std::string &key,
                  const std::string &fallback = "") const {
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  std::string required(const std::string &key) const {
    auto value = get(key);
    if (value.empty())
      throw std::invalid_argument("missing " + key);
    return value;
  }
};
Arguments parse(int argc, char **argv) {
  if (argc < 2)
    throw std::invalid_argument("a command is required; use --help");
  Arguments args;
  args.command = argv[1];
  const std::vector<std::string> commands{"mel", "detect"};
  if (std::find(commands.begin(), commands.end(), args.command) ==
      commands.end())
    throw std::invalid_argument("unknown command '" + args.command +
                                "'; use --help");
  for (int i = 2; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--expect-keyword") {
      args.expect = true;
      continue;
    }
    const std::vector<std::string> options{
        "--lang",   "--input",   "--model",      "--keyword",
        "--output", "--scores", "--sensitivities",
        "--mode",   "--finalize", "--scorer", "--scorer-options"};
    if (std::find(options.begin(), options.end(), key) == options.end())
      throw std::invalid_argument("unknown option '" + key + "'");
    if (++i == argc || std::string(argv[i]).rfind("--", 0) == 0)
      throw std::invalid_argument("missing value for " + key);
    if (key == "--keyword")
      args.keywords.push_back(argv[i]);
    else if (!args.values.emplace(key, argv[i]).second)
      throw std::invalid_argument("duplicate option " + key);
  }
  bool detection = args.command == "detect";
  if (!detection &&
      (!args.keywords.empty() || args.expect || args.values.count("--scores") ||
       args.values.count("--sensitivities") || args.values.count("--mode") ||
       args.values.count("--scorer") || args.values.count("--scorer-options")))
    throw std::invalid_argument("detector options require detect");
  if (args.command != "mel" && args.values.count("--finalize"))
    throw std::invalid_argument("--finalize is mel-only");
  if ((args.command == "mel") &&
      args.values.count("--model"))
    throw std::invalid_argument("this operation does not load a model");
  if (detection && args.values.count("--output"))
    throw std::invalid_argument(
        "detect return JSON on stdout; use --scores for arrays");
  if (args.expect && args.keywords.size() != 1)
    throw std::invalid_argument(
        "--expect-keyword requires exactly one enrollment");
  args.required("--input");
  if (args.command == "mel")
    args.required("--output");
  if (args.command == "detect")
    args.required("--model");
  if (detection && args.keywords.empty())
    throw std::invalid_argument("at least one --keyword is required");
  return args;
}
std::string read_text(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(file), {});
}
std::vector<double> sensitivities(const Arguments &args) {
  std::vector<double> values;
  std::istringstream input(args.get("--sensitivities", "0,.25,.5,.75,1"));
  std::string token;
  while (std::getline(input, token, ',')) {
    size_t used = 0;
    double value = std::stod(token, &used);
    if (used != token.size())
      throw std::invalid_argument("invalid sensitivity list");
    values.push_back(value);
  }
  if (values.empty())
    throw std::invalid_argument("empty sensitivity list");
  return values;
}
std::string quoted(const std::string &text) {
  return "\"" + kws::detail::json::escape(text) + "\"";
}
std::string metadata(const kws::LanguagePreset &lang, size_t frames,
                     size_t columns, const std::string &dtype,
                     const std::string &input_mode,
                     const std::vector<uint8_t> &resets) {
  std::ostringstream out;
  out << "{\"schema\":1,\"abi\":1,\"language\":" << lang.metadata_json
      << ",\"frames\":" << frames << ",\"columns\":" << columns
      << ",\"dtype\":" << quoted(dtype)
      << ",\"frontend_id\":\"3f047efa\",\"input_mode\":" << quoted(input_mode)
      << ",\"reset_frames\":[";
  bool first = true;
  for (size_t i = 0; i < resets.size(); ++i)
    if (resets[i]) {
      if (!first)
        out << ',';
      out << i;
      first = false;
    }
  out << "]}";
  return out.str();
}
void write_file(const std::string &path, const void *data, size_t bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output)
    throw std::runtime_error("cannot write " + path);
  if (bytes)
    output.write(static_cast<const char *>(data), bytes);
  output.close();
  if (!output)
    throw std::runtime_error("failed writing " + path);
}
template <class T>
void save(const std::string &path, const std::vector<T> &values,
          const std::string &meta) {
  write_file(path, values.data(), values.size() * sizeof(T));
  write_file(path + ".json", meta.data(), meta.size());
}
int run(const Arguments &args) {
  const auto &lang = kws::language_preset(args.get("--lang", "en"));
  if (args.command == "mel") {
    const auto policy = args.get("--finalize", "training_nearest_hop");
    if (policy != "complete_hops" && policy != "training_nearest_hop")
      throw std::invalid_argument("unknown mel finalization policy");
    auto wav = kws::detail::load_wav(args.required("--input"));
    size_t capacity = wav.pcm.size() / 256 + 1;
    std::vector<int16_t> mel(capacity * 40);
    kws::Frontend frontend;
    size_t frames =
        frontend.push(wav.pcm.data(), wav.pcm.size(), mel.data(), capacity);
    frames += frontend.finish(policy == "complete_hops"
                                  ? kws::Finalization::complete_hops
                                  : kws::Finalization::training_nearest_hop,
                              mel.data() + frames * 40);
    mel.resize(frames * 40);
    auto meta = metadata(lang, frames, 40, "<i2", "pcm_mel_" + policy, {});
    save(args.required("--output"), mel, meta);
    std::cout << meta << '\n';
    return 0;
  }
  const bool detection = true;
  auto points = sensitivities(args);
  kws::DetectorConfig config;
  if (detection) {
    std::string json = "[";
    for (const auto &path : args.keywords) {
      if (json.size() != 1)
        json += ',';
      json += read_text(path);
    }
    json += ']';
    auto mode = args.get("--mode", "independent");
    if (mode != "independent" && mode != "production_arbitrated")
      throw std::invalid_argument("unknown keyword mode");
    config = kws::parse_enrollments(
        json.c_str(), points.data(), points.size(),
        mode == "independent" ? kws::KeywordMode::independent
                              : kws::KeywordMode::production_arbitrated,
        lang.classes(), &lang);
    if (args.values.count("--scorer") || args.values.count("--scorer-options"))
      for (auto &e : config.enrollments) {
        e.scorer = args.get("--scorer", e.scorer);
        e.scorer_options = args.get("--scorer-options", "");
      }
  }
  auto model = kws::ModelWeights::load(args.required("--model"), lang.code);
  auto wav = kws::detail::load_wav(args.required("--input"));
  size_t frames = wav.pcm.size() / 512;
  std::vector<int16_t> scores(frames * args.keywords.size());
  std::vector<uint8_t> events(frames * args.keywords.size() * points.size());
  std::vector<uint8_t> resets(frames);
  kws::Session session(model, std::make_unique<kws::Detector>(config));
  session.push_pcm(wav.pcm.data(), wav.pcm.size(),
                   {frames, nullptr, resets.data(), scores.data(), nullptr,
                    events.data(), nullptr});
  size_t discarded = session.finish();
  if (args.values.count("--scores"))
    save(args.get("--scores"), scores,
         metadata(lang, frames, args.keywords.size(), "<i2", args.command,
                  resets));
  std::cout << "{\"language\":" << lang.metadata_json
            << ",\"frames\":" << frames
            << ",\"discarded_samples\":" << discarded
            << ",\"operating_points\":[";
  for (size_t s = 0; s < points.size(); ++s) {
    if (s)
      std::cout << ',';
    std::cout << "{\"sensitivity\":" << points[s] << ",\"detections\":[";
    bool first = true;
    for (size_t t = 0; t < frames; ++t)
      for (size_t k = 0; k < args.keywords.size(); ++k)
        if (events[(t * points.size() + s) * args.keywords.size() + k]) {
          if (!first)
            std::cout << ',';
          std::cout << "{\"frame\":" << t
                    << ",\"time_seconds\":" << std::setprecision(12)
                    << (t + 1) * .032
                    << ",\"keyword\":" << quoted(config.enrollments[k].name)
                    << '}';
          first = false;
        }
    std::cout << ']';
    if (args.expect)
      std::cout << ",\"hit\":" << (first ? "false" : "true")
                << ",\"miss\":" << (first ? "true" : "false");
    std::cout << '}';
  }
  std::cout << "]}\n";
  return 0;
}
} // namespace
int main(int argc, char **argv) {
  try {
    for (int i = 1; i < argc; ++i)
      if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
        std::cout << help;
        return 0;
      }
    if (argc == 2 && std::string(argv[1]) == "--list-languages") {
      for (const auto &preset : kws::supported_languages())
        std::cout << preset.code << '\t' << preset.classes() - 1
                  << " phones + blank"
                  << (preset.code == "en" ? " (default)" : "") << '\n';
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--list-scorers") {
      for (const auto &name : kws::detail::scorer_names())
        std::cout << name << (name == "production" ? " (default)" : "") << '\n';
      return 0;
    }
    return run(parse(argc, argv));
  } catch (const std::invalid_argument &error) {
    std::cerr << error.what() << '\n';
    return 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
