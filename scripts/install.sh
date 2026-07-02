#!/bin/bash
# Deploy Fizzik to the Move over SSH.
set -e
MODULE_ID="fizzik"
MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/sound_generators/$MODULE_ID"

echo "Installing $MODULE_ID to $MOVE_HOST..."
ssh "ableton@$MOVE_HOST" "mkdir -p $DEST"
scp "dist/$MODULE_ID/dsp.so" "dist/$MODULE_ID/module.json" "ableton@$MOVE_HOST:$DEST/"
ssh "ableton@$MOVE_HOST" "chmod +x $DEST/dsp.so && chown -R ableton:users $DEST"
echo "Done. Power-cycle the Move (or remove/re-add the module) to reload module.json."
echo "Verify: ssh ableton@$MOVE_HOST 'ls -la $DEST/'"
