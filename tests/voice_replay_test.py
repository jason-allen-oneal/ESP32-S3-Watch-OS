#!/usr/bin/env python3
"""Execute production replay ownership paths with a controlled audio queue under sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / "components/nightglass_services/src/voice.cpp").read_text()
def function(signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        if source[end] == "{": depth += 1
        elif source[end] == "}": depth -= 1
        end += 1
    return source[start:end]
PREFIX = r"""#include "nightglass/services/voice.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cassert>
using namespace nightglass::services;
constexpr int pdTRUE=1,portMAX_DELAY=0;
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
int xSemaphoreTake(void*,int){return pdTRUE;}
void xSemaphoreGive(void*){}
unsigned frees=0;
void heap_caps_free(void*p){++frees;std::free(p);}
namespace nightglass::services {
struct VoicePlaybackResult{nightglass::core::StatusCode status;};
struct TestAudio {
 bool fail=false;void*context=nullptr;void(*callback)(void*,const VoicePlaybackResult&)=nullptr;
 nightglass::core::Status request_voice_playback(std::uint8_t*,std::size_t,void(*cb)(void*,const VoicePlaybackResult&),void*c){
  if(fail)return {nightglass::core::StatusCode::io_error,"queue full"};context=c;callback=cb;return nightglass::core::Status::Ok();
 }
 void stop_voice_playback(){if(callback){auto cb=callback;callback=nullptr;cb(context,{nightglass::core::StatusCode::unavailable});}}
} test_audio;
TestAudio&audio_service(){return test_audio;}
}
VoiceSnapshot current{};void*response_mutex=(void*)1;int voice_lock=0;
std::atomic_bool cancel_requested{false},update_blocked{false};
std::uint8_t *replay_buffer=nullptr,*audio_response_buffer=nullptr;
std::size_t replay_bytes=0;
bool audio_response_streaming=false,audio_stream_ended=false,audio_stream_failed=false,audio_playback_active=false;
unsigned audio_response_id=0,audio_response_expected=0,audio_response_received=0;
unsigned response_id=0,response_expected=0,response_received=0,response_crc=0;
struct VoicePlaybackContext {unsigned session_id=0;std::uint8_t*buffer=nullptr;std::size_t bytes=0;long long stream_deadline_us=0;};
VoicePlaybackContext playback_context;
"""
SUFFIX = r"""int main(){
 VoiceService service;
 current.session_id=42;current.state=VoiceTurnState::speaking;audio_playback_active=true;
 playback_context={42,(std::uint8_t*)std::malloc(16),16,0};
 voice_playback_complete(&playback_context,{nightglass::core::StatusCode::ok});
 assert(current.replay_available&&current.state==VoiceTurnState::complete&&frees==0);
 assert(service.replay_reply().is_ok());assert(audio_playback_active&&!current.replay_available);
 assert(!service.replay_reply().is_ok()); // no double ownership / concurrent replay
 test_audio.callback(test_audio.context,{nightglass::core::StatusCode::ok});test_audio.callback=nullptr;
 assert(current.replay_available&&frees==0);
 test_audio.fail=true;assert(!service.replay_reply().is_ok());
 assert(current.replay_available&&current.state==VoiceTurnState::complete&&frees==0);
 test_audio.fail=false;assert(service.replay_reply().is_ok());
 cancel_requested=true;current.state=VoiceTurnState::cancelled;test_audio.stop_voice_playback();
 assert(!current.replay_available&&replay_buffer==nullptr&&frees==1);
 cancel_requested=false;current.state=VoiceTurnState::speaking;audio_playback_active=true;
 playback_context={42,(std::uint8_t*)std::malloc(16),16,0};
 voice_playback_complete(&playback_context,{nightglass::core::StatusCode::ok});
 assert(current.replay_available);wipe_response();assert(!current.replay_available&&frees==2);
 assert(!service.replay_reply().is_ok());
}
"""
code = PREFIX + "\n".join(function(name) for name in (
    "void secure_wipe(", "void clear_replay_locked(", "void wipe_response(",
    "void voice_playback_complete(", "nightglass::core::Status VoiceService::replay_reply(")) + SUFFIX
with tempfile.TemporaryDirectory(prefix="nightglass-replay-") as directory:
    cpp = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    cpp.write_text(code)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-fsanitize=address,undefined", "-g",
        "-I" + str(root / "components/nightglass_services/include"),
        "-I" + str(root / "components/nightglass_core/include"), str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Voice replay ownership passed: successful repeat, duplicate prevention, enqueue failure, cancellation and wipe")
