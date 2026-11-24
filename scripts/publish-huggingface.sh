#!/usr/bin/env bash
# Publishes Phonlet Wake on Hugging Face: the model repo phonletai/wake-en-1, the static Space
# phonletai/wake (the lab) and the organisation card (the phonletai/README Space). The model and
# the lab are built from the files of a GitHub release, checked against its SHA256SUMS, plus
# packaging/huggingface/ from this repository.
#
#   scripts/publish-huggingface.sh --stage DIR   build both trees into DIR/model and DIR/space only
#   scripts/publish-huggingface.sh               build and upload (needs `hf auth login` with write
#                                                access to the phonletai organisation)
set -euo pipefail
cd "$(dirname "$0")/.."

RELEASE=v0.1.0
ORG=phonletai
MODEL_REPO=$ORG/wake-en-1
SPACE_REPO=$ORG/wake
CARD_REPO=$ORG/README
URL=https://github.com/phonlet-ai/wake/releases/download/$RELEASE
FILES=(phonlet-wake-en-1.pwake phonlet-wake-en-1-keywords.json wake-threshold-0.2.0.wasm
  phonlet-ai-wake-web-0.1.0.tgz)

stage=""
if [[ ${1:-} == --stage ]]; then
  stage=${2:?usage: --stage DIR}
  mkdir -p "$stage"
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# Release files, verified.
mkdir -p "$work/release"
curl -fsSL -o "$work/release/SHA256SUMS" "$URL/SHA256SUMS"
for file in "${FILES[@]}"; do curl -fsSL -o "$work/release/$file" "$URL/$file"; done
(cd "$work/release" && for file in "${FILES[@]}"; do grep " $file\$" SHA256SUMS; done | sha256sum -c --quiet)

# The model repo: card, model, English phrases, licence and notices.
model=$work/model
mkdir -p "$model"
cp packaging/huggingface/model/README.md LICENSE THIRD_PARTY_NOTICES.md "$model/"
cp "$work/release/phonlet-wake-en-1.pwake" "$work/release/phonlet-wake-en-1-keywords.json" "$model/"

# The Space: the page, the browser SDK with the model, and the generator.
space=$work/space
mkdir -p "$space/wake" "$space/generator"
cp -r packaging/huggingface/space/. "$space/"
tar -xzf "$work/release/phonlet-ai-wake-web-0.1.0.tgz" -C "$work"
cp -r "$work/package/." "$space/wake/"
cp "$work/release/phonlet-wake-en-1.pwake" "$space/wake/model.pwake"
cp "$work/release/phonlet-wake-en-1-keywords.json" "$space/wake/keywords.json"
cp "$work/release/wake-threshold-0.2.0.wasm" "$space/generator/wake_threshold.wasm"
cp LICENSE "$space/generator/LICENSE"
cp generator/THIRD_PARTY_NOTICES.md "$space/generator/THIRD_PARTY_NOTICES.md"

if [[ -n $stage ]]; then
  rm -rf -- "$stage/model" "$stage/space"
  cp -r "$model" "$space" "$stage/"
  echo "Staged $stage/model and $stage/space (nothing uploaded)."
  exit 0
fi

# The hf CLI's commands change between versions: pin the one this script is written for.
hf() { uvx -q --from huggingface_hub==2.4.0 hf "$@"; }
hf auth whoami >/dev/null
hf repos create "$MODEL_REPO" --repo-type model --public --exist-ok
hf upload "$MODEL_REPO" "$model" . --repo-type model \
  --commit-message "Phonlet Wake en-1 from release $RELEASE"
hf repos create "$SPACE_REPO" --repo-type space --space-sdk static --public --exist-ok
hf upload "$SPACE_REPO" "$space" . --repo-type space \
  --commit-message "Phonlet Wake lab from release $RELEASE"
hf repos create "$CARD_REPO" --repo-type space --space-sdk static --public --exist-ok
# The card shows the Space's index.html when it has one, so the template's files are removed.
hf upload "$CARD_REPO" packaging/huggingface/organisation . --repo-type space \
  --delete index.html --delete style.css --commit-message "Organisation card"
echo "Published https://huggingface.co/$MODEL_REPO, https://huggingface.co/spaces/$SPACE_REPO and https://huggingface.co/$ORG"
