#!/bin/bash
# Acceptance check on a connected dog: runs each capability through the device shell
# ("gadget <command> [json]") and checks the reply. The dog moves and makes sound.
#
# usage: tools/device_check.sh [--no-motion] [--no-voice]
#   ADB=/path/to/adb (default: adb)  SERIAL=<the dog's serial in `adb devices`>
set -u
ADB=${ADB:-adb}
SERIAL=${SERIAL:?set SERIAL to the serial of the dog in adb devices}
MOTION=1
VOICE=1
for a in "$@"; do
    case $a in
        --no-motion) MOTION=0 ;;
        --no-voice) VOICE=0 ;;
    esac
done
OUT=$(mktemp -d)
pass=0
fail=0

# shell <seconds> <line>: run one line in the device shell, keep the output for <seconds>.
shell() {
    (sleep 1; printf '%s\n' "$2"; sleep "$1") | timeout $(($1 + 3)) "$ADB" -s "$SERIAL" shell 2>&1 |
        LC_ALL=C sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$OUT/last"
}

# check <name> <seconds> <line> <regex>: the output must match the regex.
check() {
    shell "$2" "$3"
    if LC_ALL=C grep -aqE "$4" "$OUT/last"; then
        printf 'PASS  %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s\n      sent: %s\n      want: %s\n' "$1" "$3" "$4"
        LC_ALL=C grep -aE 'gadget_sh\]|gadget_voice|error|failed' "$OUT/last" | tail -6 | sed 's/^/      got:  /' | cut -c1-200
        fail=$((fail + 1))
    fi
}

"$ADB" -s "$SERIAL" get-state > /dev/null 2>&1 || { echo "no device $SERIAL on adb"; exit 2; }

echo "== device"
check "version is 0.1.0" 2 "version" "impo-gadget-arcs 0\.1\.0"
check "device.health: official fields" 2 "gadget device.health" '"uptime_seconds":[0-9]+.*"heap_free_internal".*"battery_pct".*"version":"0\.1\.0"'
check "online in Impo" 2 "gadget device.health" '"network_ssid":"'

echo "== motion sensor"
check "imu.read: orientation word" 2 "gadget imu.read" '"orientation":"(upright|upside_down|face_up|face_down|on_left_side|on_right_side|tilted)"'
check "imu.read: accel and gyro" 2 "gadget imu.read" '"accel_g":\{"x":.*"gyro_dps":\{'

echo "== light"
check "set_light breathe_fast purple" 2 'gadget chassis.set_light {"effect":"breathe_fast","color":"#a000ff"}' '"ok":true,"payload":\{"base":"responding"'
check "set_light flowing" 2 'gadget chassis.set_light {"effect":"flowing","color":"#00ff40"}' '"ok":true'
check "set_light show (rainbow)" 2 'gadget chassis.set_light {"effect":"show"}' '"ok":true'
check "set_light refuses an unknown effect" 2 'gadget chassis.set_light {"effect":"disco"}' '"ok":false,"error":"..effect.." must be a string, one of'
check "set_light refuses a bad colour" 2 'gadget chassis.set_light {"effect":"on","color":"red"}' '"ok":false,"error":"..color.. must be a string of exactly 7 characters'
check "display.show_animation" 2 "gadget display.show_animation" '"ok":true'

echo "== speaker"
check "speaker.set_volume 60" 2 'gadget speaker.set_volume {"percent":60}' '"percent":60'
check "speaker.set_volume refuses 150" 2 'gadget speaker.set_volume {"percent":150}' '"ok":false'
check "speaker.beep" 2 "gadget speaker.beep" '"ok":true'
check "speaker.say starts fetching" 2 'gadget speaker.say {"text":"Device check. Hello!"}' '"status":"fetching"'
check "speaker.say speech arrives" 8 "gadget speaker.status" 'speech: [0-9]+ Hz|"status":"(playing|idle)"'
check "speaker.say refuses empty text" 2 'gadget speaker.say {"text":""}' '"ok":false'
check "speaker.play_url plays an MP3" 8 'gadget speaker.play_url {"url":"https://interactive-examples.mdn.mozilla.net/media/cc0-audio/t-rex-roar.mp3"}' 'MP3: 44100 Hz'
check "speaker.status after play_url" 4 "gadget speaker.status" '"status":"idle".*"speaker_on":true.*"last_url":"https://interactive'
check "speaker.play_url refuses http" 2 'gadget speaker.play_url {"url":"http://example.com/a.mp3"}' '"ok":false,"error":"url must be https'
check "speaker.play_url reports a 404" 6 'gadget speaker.play_url {"url":"https://interactive-examples.mdn.mozilla.net/media/cc0-audio/nope.mp3"}' 'the server refused the file'
check "speaker.stop" 2 "gadget speaker.stop" '"ok":true'

if [ $MOTION = 1 ]; then
    echo "== motion (the dog moves)"
    check "chassis.drive forward" 8 'gadget chassis.drive {"forward":0.5,"duration_ms":1300}' 'forward x[0-9]+: 0'
    check "chassis.drive turn" 7 'gadget chassis.drive {"turn":-0.6,"duration_ms":1200}' 'turn_left x[0-9]+: 0'
    check "chassis.drive {} stops" 3 'gadget chassis.drive {}' '"action":"stop"'
    check "chassis.move back" 6 'gadget chassis.move {"direction":"back","duration_ms":1300}' 'backward x[0-9]+: 0'
    check "chassis.status" 2 "gadget chassis.status" '"base":"responding","moving":(true|false).*"orientation":"upright"'
    check "dog.trick paw" 9 'gadget dog.trick {"trick":"paw"}' 'idle_scratch x0: 0'
    check "dog.trick handshake" 7 'gadget dog.trick {"trick":"handshake"}' 'right_front_handshake x0: 0'
    check "dog.trick upside_wiggle refused upright" 2 'gadget dog.trick {"trick":"upside_wiggle"}' 'needs the dog on its back'
    check "dog.trick refuses an unknown trick" 2 'gadget dog.trick {"trick":"moonwalk"}' '"ok":false'
    check "chassis.move refuses a string duration" 2 'gadget chassis.move {"direction":"left","duration_ms":"3000"}' 'must be an integer from 1 to 10000.*got ..3000'
    check "chassis.move refuses an unknown parameter" 2 'gadget chassis.move {"direction":"left","speed":3}' 'unknown parameter ..speed..: this command takes direction, duration_ms, fast'
    check "chassis.move right for 10 s starts" 3 'gadget chassis.move {"direction":"right","duration_ms":10000}' 'turn_right x17: 0'
    check "chassis.stop interrupts it at once" 3 "gadget chassis.stop" 'stop: 0'
    check "a stopped dog is idle" 2 "gadget chassis.status" '"moving":false'
    check "dog.trick spin ends by itself" 14 'gadget dog.trick {"trick":"spin"}' 'motion finished: spin'
    check "dog.pose sleep" 6 'gadget dog.pose {"pose":"sleep"}' 'standby_fold x0: 0'
fi

if [ $VOICE = 1 ]; then
    echo "== voice (the dog asks itself a question out loud)"
    check "voice turn: heard, sent, answered" 45 "talk_test What is two plus three? Answer in one short sentence." 'reply: .*(five|5)'
fi

echo "== $pass passed, $fail failed"
rm -rf "$OUT"
[ $fail = 0 ]
