// ============================================================================
// src/ext/render_pipe/pipe_driver.h -- RENDERPIPE-1: the frame orchestration
// (fence, drains, hand-off), sequential or on a render thread
// ============================================================================
// NOT RETAIL. ext.render_pipe (docs/extensions/handins/RENDERPIPE1.md).
//
// Engine-free: the engine's Host lives in render_pipe.cpp, the unit test's in
// tests/unit/ext_render_pipe_test.cpp (with a std::thread as the render thread).
//
// Two modes, one state machine (pipe_state.h, depth 1):
//   SEQUENTIAL (no Handoff; RENDERPIPE-0, and every machine with fewer than
//     three physical cores): "join" = emit the held packet on the main thread,
//     in the pipelined order (pass 1 of N+1, then the packet of N).
//   THREADED (a Handoff and a render thread running WorkerLoop): the packet is
//     PUBLISHED at the Frame tail that held it and the render thread emits it
//     while the main thread goes on (the next Frame's prelude, and the main half
//     of N+1 when that Frame does not step). "join" = wait until the render
//     thread is idle, then read its result.
//
// Every join site (THE FENCE before a step, the drain of every transition,
// the hand-off at a pipelined tail) is followed by main-thread code that may
// write what the emit reads; the join is the acquire that orders them
// (handoff.h). Device lost in the threaded mode: the render thread only
// PRESENTS and reports the failure (EmitResult::recover); the recovery runs
// here, on the main thread, at the join (Host::Recover), where retail runs it
// (inside MainLoop::Draw, on the main thread, with nothing else in flight).
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>

#include "ext/render_pipe/handoff.h"
#include "ext/render_pipe/packet.h"
#include "ext/render_pipe/pipe_state.h"
#include "ext/render_pipe/render_pipe.h"   // Counters

namespace Ext {
namespace RenderPipe {

// What one emit reports to the join.
struct EmitResult {
    bool    failed    = false;   // BeginCommands failed (retail's DrawFailed path ran in the emit)
    bool    lost      = false;   // the device was lost in Finish
    bool    recover   = false;   // threaded: the recovery half is still to run, on the main thread
    int32_t hr        = 0;       // the Present / removed-reason result the recovery reports
    int64_t presentUs = 0;       // NowUs() after Finish (the packet latency)
};

inline int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Host (main thread unless stated):
//   int32_t    ContextFrameIndex();               Context::_frameIndex (only read when nothing is pending)
//   void       MainHalf(Packet& next, const TailPlan& plan);   steps 1-6 + pass 1 into next.slot
//   EmitResult Emit(const Packet& p, bool threaded);           the EMITTING thread: render (threaded) or main
//   void       Recover(const EmitResult& r);                   the device-lost recovery half (threaded only)
//   void       AfterJoin(const Packet& next);                  render idle, a skipping frame kept: the camera shadow
//   void       FaithfulTail();                                 MainLoop::Frame's own two tail statements
template <class Host>
class Driver {
public:
    // Threaded when `h` is non-null (its render thread runs WorkerLoop).
    void Attach(Handoff* h) { _handoff = h; }
    bool Threaded() const { return _handoff != nullptr; }
    const PipeState& State() const { return _state; }

    void Reset() {
        _state     = PipeState{};
        _held      = Packet{};
        _result    = EmitResult{};
        _publishUs = 0;
    }

    Counters TakeWindow() {
        const Counters c = _counters;
        _counters = Counters{};
        return c;
    }

    // kExtBeforeSteps: THE FENCE. The sim never runs with a packet in flight.
    void BeforeSteps(Host& host) {
        if (_state.DrainBeforeSteps(true)) {
            ++_counters.drainStep;
            Join(host, false);
        }
    }

    // kExtFrameTail. True: the extension was switched off -- uninstall now (the
    // render thread is idle and joined).
    bool Tail(Host& host, const TailInputs& in) {
        const TailPlan plan = _state.PlanTail(in);
        if (plan.drain) {
            ++_counters.drainOther;
            Join(host, false);
        }
        if (!plan.pipelined) {
            ++_counters.sequential;
            host.FaithfulTail();
            _state.FaithfulFrame();
            return plan.uninstall;
        }
        Pipelined(host, plan);
        return false;
    }

    // The render thread's body: park, emit, report, until Stop.
    void WorkerLoop(Host& host) {
        while (_handoff->WaitJob()) {
            _result = host.Emit(_held, true);
            _result.presentUs = NowUs();
            _handoff->Done();
        }
    }

private:
    // Join the pending packet: emit it here (sequential) or wait for the
    // render thread (threaded). False: the frame gathered since must not be
    // emitted (the device was lost, or BeginCommands failed).
    bool Join(Host& host, bool handoff) {
        EmitResult res;
        if (_handoff != nullptr) {
            const int64_t t0 = NowUs();
            const bool waited = _handoff->WaitIdle();   // ACQUIRE: the emit's writes are visible
            if (waited) {
                ++_counters.waits;
                if (!handoff) ++_counters.waitsFence;
                _counters.waitUs += static_cast<uint64_t>(NowUs() - t0);
            }
            res = _result;
            if (res.presentUs >= _publishUs) {
                _counters.latencyUs += static_cast<uint64_t>(res.presentUs - _publishUs);
                ++_counters.latencyN;
            }
        } else {
            res = host.Emit(_held, false);
        }
        _state.Drained();
        ++_counters.emits;
        if (res.recover) {
            ++_counters.recoveries;
            host.Recover(res);   // the MAIN thread, the render thread idle
        }
        return !res.failed && !res.lost;
    }

    void Pipelined(Host& host, const TailPlan& plan) {
        const bool pending = _state.Pending();
        Packet next;
        next.slot = plan.pass1Slot;
        // Fix 2: the index the context holds when this packet's pass 2 runs. With
        // a packet pending, that packet's emit increments it first; the context is
        // read only when nothing is in flight (the render thread may be writing it).
        next.frameIndex = pending ? _held.frameIndex + 1 : host.ContextFrameIndex();
        next.modelIndex = plan.renderIdle ? host.ContextFrameIndex() : _held.modelIndex + 1;

        host.MainHalf(next, plan);   // threaded: concurrent with the emit of _held
        ++_counters.pipelined;
        if (plan.renderIdle) ++_counters.idleWrites; else ++_counters.idleSkips;

        bool keep = true;
        if (pending) keep = Join(host, true);   // THE HAND-OFF: wait for N, then N+1 goes
        if (!keep) {
            _state.Discard(next.slot);
            ++_counters.discarded;
            return;
        }
        if (!plan.renderIdle) host.AfterJoin(next);
        _held = next;
        _state.Hold(next.slot);
        if (_handoff != nullptr) {
            _publishUs = NowUs();
            _handoff->Publish();   // RELEASE: _held is written before
        }
    }

    PipeState  _state;
    Packet     _held;              // the pending packet (the render thread reads it while busy)
    EmitResult _result;            // written by the emitting thread before Done
    Handoff*   _handoff   = nullptr;
    int64_t    _publishUs = 0;
    Counters   _counters;          // main thread only
};

}  // namespace RenderPipe
}  // namespace Ext
