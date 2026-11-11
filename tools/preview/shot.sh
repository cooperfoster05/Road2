#!/bin/bash
# shot.sh <scene> [frames]  -> shots/<scene>.png (cropped to the window)
cd /home/claude/h; mkdir -p shots
./prev "$1" /tmp/claude-0/o.rgba ${2:-8} || { echo CRASH; exit 1; }
python3 - "$1" <<'PY'
import sys
from PIL import Image
im=Image.frombytes('RGBA',(1920,1080),open('/tmp/claude-0/o.rgba','rb').read()).convert('RGB')
im.save('/home/claude/h/shots/%s_full.png'%sys.argv[1])
PY
python3 -c "
from PIL import Image
im=Image.open('/home/claude/h/shots/$1_full.png').crop((30,10,600,1080)); im.save('/home/claude/h/shots/$1.png')"
