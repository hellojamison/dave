// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/Types.h"

#include <atomic>

namespace dave::engine {

// Transport owns playback state: play/stop, the timeline position, and loop
// points. The UI thread mutates play state and requests seeks; the RT thread
// advances the sample counter and writes the current TimeInfo each block.
//
// The transport is NOT part of the node graph — it's a peer that the AudioEngine
// reads each block to fill TimeInfo before calling CompiledGraph::process().
// Nodes that care about time (AudioClipNode) read ctx.time->samplePos; nodes
// that don't (SineNode) ignore it.
class Transport {
public:
    Transport() = default;

    // --- UI thread ---------------------------------------------------------
    // Play/stop are a matched pair: stopping returns the cursor to wherever
    // this pass started, the Pro Tools default. Playback is for auditioning,
    // so the common case is listening to a spot and then editing at it — and
    // leaving the cursor wherever the music happened to reach means finding
    // that spot again by hand every single time.
    void play() {
        if (!playing_.exchange(true, std::memory_order_acq_rel)) {
            returnPosition_.store(effectivePosition(), std::memory_order_release);
        }
    }
    void stop() {
        if (playing_.exchange(false, std::memory_order_acq_rel)) {
            seek(returnPosition_.load(std::memory_order_acquire));
        }
    }
    void toggle() {
        if (isPlaying()) stop(); else play();
    }
    // A seek is the user saying "start from here", so it moves the return
    // point too — including mid-playback, where the alternative is stopping
    // and being thrown back to a position two jumps ago.
    void seek(int64_t samplePos) {
        nextPosition_.store(samplePos, std::memory_order_release);
        returnPosition_.store(samplePos, std::memory_order_release);
    }
    bool isPlaying() const { return playing_.load(std::memory_order_acquire); }
    int64_t position() const { return currentPosition_.load(std::memory_order_acquire); }
    // Where stop() will put the cursor.
    int64_t returnPosition() const {
        return returnPosition_.load(std::memory_order_acquire);
    }

    void setLoop(int64_t start, int64_t end) {
        loopStart_ = start;
        loopEnd_ = end;
        looping_.store(start < end, std::memory_order_release);
    }
    void clearLoop() {
        looping_.store(false, std::memory_order_release);
    }

    // --- RT thread ---------------------------------------------------------
    // Called once per block by the AudioEngine BEFORE graph processing. Advances
    // the position by numSamples (honoring loop), fills `out` for the graph.
    void advanceAndFill(TimeInfo& out, int numSamples, double sampleRate) {
        // Pick up a pending seek (UI thread requested it via seek()).
        int64_t requested = nextPosition_.load(std::memory_order_acquire);
        int64_t pos = currentPosition_.load(std::memory_order_relaxed);
        if (requested >= 0 && requested != pos) {
            pos = requested;
            nextPosition_.store(-1, std::memory_order_release); // consume
        }

        out.sampleRate = sampleRate;
        out.samplePos = pos;
        // Musical position, for plugins with tempo-synced anything. Dave has no
        // tempo map yet, so this is a straight conversion at a constant tempo —
        // a placeholder that is *correct* for the constant-tempo case rather
        // than a zero that tells every synced plugin the session never moves.
        // When the tempo map lands (see MidiClip::sourceTempi) this becomes a
        // lookup and the rest of the chain is already wired for it.
        out.bpm = kPlaceholderBpm;
        out.ppqPosition = (sampleRate > 0.0)
            ? (static_cast<double>(pos) / sampleRate) * (kPlaceholderBpm / 60.0)
            : 0.0;
        out.isPlaying = playing_.load(std::memory_order_acquire);
        out.isLooping = looping_.load(std::memory_order_acquire);
        out.loopStart = loopStart_;
        out.loopEnd = loopEnd_;

        // Advance position for the NEXT block (this block covers [pos, pos+n)).
        // We MUST persist currentPosition_ even when stopped — the UI reads it
        // to draw the playhead, and seeks while stopped need to be reflected.
        if (out.isPlaying) {
            int64_t next = pos + numSamples;
            if (out.isLooping && next >= out.loopEnd) {
                next = out.loopStart + (next - out.loopEnd);
            }
            currentPosition_.store(next, std::memory_order_relaxed);
        } else {
            // Stopped: still persist the (possibly seeked) position so the UI
            // sees it. Without this, position() returns stale data after a seek
            // while stopped, and the playhead never appears to move.
            currentPosition_.store(pos, std::memory_order_relaxed);
        }
    }

private:
    // One session tempo until a real tempo map exists.
    static constexpr double kPlaceholderBpm = 120.0;

    // Where the cursor is *going* to be, which is not currentPosition_ while a
    // seek is still queued for the RT thread. play() has to read this one, or
    // pressing play right after a click records the position the click just
    // left rather than the one it landed on.
    int64_t effectivePosition() const {
        const int64_t pending = nextPosition_.load(std::memory_order_acquire);
        return pending >= 0 ? pending
                            : currentPosition_.load(std::memory_order_acquire);
    }

    std::atomic<bool> playing_{false};
    std::atomic<int64_t> currentPosition_{0};
    std::atomic<int64_t> nextPosition_{-1}; // >=0 means a seek is pending
    std::atomic<int64_t> returnPosition_{0}; // where stop() puts the cursor
    std::atomic<bool> looping_{false};
    int64_t loopStart_ = 0;
    int64_t loopEnd_ = 0;
};

} // namespace dave::engine
