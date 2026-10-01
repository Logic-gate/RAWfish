// Exercise the actual QML bracket selection functions without a Qt build.
const assert=require('assert'), fs=require('fs'), vm=require('vm'), path=require('path');
const read=p=>fs.readFileSync(path.join(__dirname,'..',p),'utf8');
const source=read('src/capture/CaptureView.qml');
const c={Settings:{mode:{camera2CaptureFormat:'raw',rawCaptureBracket:'off'}}};vm.createContext(c);
vm.runInContext(source.match(/    function _camera2BracketSteps\(\) \{[\s\S]*?\n    \}/)[0],c);
for(const [setting,expected] of [['off',[]],['ev1',[0,-2]],['ev2',[0,-4]],['ev3',[0,-6]]]) {
 c.Settings.mode.rawCaptureBracket=setting;
 assert.equal(JSON.stringify(c._camera2BracketSteps()),JSON.stringify(expected));
}
// A six-stop pair keeps the base exposure and requests 1/64 of its shutter time.
assert.equal(c._camera2BracketSteps()[0],0);
assert.equal(64000000*Math.pow(2,c._camera2BracketSteps()[1]),1000000);
for(const setting of ['off','ev1','ev2','ev3']) {
 c.Settings.mode.rawCaptureBracket=setting;
 c.Settings.mode.camera2CaptureFormat='jpeg';
 assert.equal(c._camera2BracketSteps().length,0);
}
const labels={};vm.createContext(labels);
vm.runInContext(read('src/settings.qml').match(/    function rawCaptureBracketLabel\(mode\) \{[\s\S]*?\n    \}/)[0],labels);
assert.equal(labels.rawCaptureBracketLabel('ev3'),'6-stop pair');
assert(!source.includes('combineBracketImages'));
assert(source.includes('if (captureView._warmRawReady) {'));
assert(source.includes('onRawBracketFrameReady:'));
const bridge=read('sfos-camera2-bridge/android/preview.c');
assert(!bridge.includes('raw_bracket_next_image'));
assert(bridge.includes('context->raw_bracket_result[i].timestamp_ns == timestamp'));
assert(bridge.includes('command.bracket_count == 2'));
assert(!bridge.includes('context->raw_bracket_requests[index] == request'));
assert(bridge.includes('ACaptureRequest_getConstEntry(request, ACAMERA_SENSOR_EXPOSURE_TIME'));
assert(bridge.includes('capture->generation != atomic_load_explicit'));
assert(source.includes('_camera2BracketQueue.length > 0 || _camera2BracketMerging'));
assert(read('src/camera2preview.cpp').includes('if (metadataIndex < 0) return;'));
assert(read('src/camera2preview.cpp').includes('count != 2 || count != metadataPaths.count()'));
const extensions=read('src/declarativecameraextensions.cpp');
assert(!extensions.includes('combineBracketJpegs'));
assert(extensions.includes('model.append(QStringLiteral("ev3"))'));
// Non-build checks: the native helper now accepts -6 and retains range clamping.
const shutterHelper=extensions.split('QString DeclarativeCameraExtensions::camera2BracketShutterNs(')[1].split('QStringList DeclarativeCameraExtensions::camera2ShutterModel')[0];
assert(shutterHelper.includes('ev < -6'));
assert(shutterHelper.includes('qBound(range[0].toDouble(),'));
assert(shutterHelper.includes('time * std::pow(2.0, ev), double(m_camera2MaxShutterNs)'));
assert(source.includes('Shutter limit: reduced bracket separation'));
assert(extensions.includes('if (m_bracketCount == 2) return stageRawBracketFrame();'));
console.log('Bracket selection, JPEG exclusion and capture wiring checks passed.');

const parser=read('sfos-camera2-bridge/android/preview_commands.h');
assert(bridge.includes('return preview_parse_capture_command(line, command);'));
assert(!bridge.includes('sscanf(line, "capture-raw '));
assert(parser.includes('!strcmp(name, "capture-raw")'));
assert(parser.includes('!strcmp(name, "capture-raw-bracket")'));
assert(bridge.includes('"command-error"'));
assert(read('src/camera2preview.cpp').includes('frames received)'));
