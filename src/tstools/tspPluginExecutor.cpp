//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2019, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  Transport stream processor: Execution context of a plugin
//
//----------------------------------------------------------------------------

#include "tspPluginExecutor.h"
#include "tsPluginRepository.h"
#include "tsGuardCondition.h"
#include "tsGuard.h"
TSDUCK_SOURCE;


//----------------------------------------------------------------------------
// Constructors and destructors.
//----------------------------------------------------------------------------

ts::tsp::PluginExecutor::PluginExecutor(Options* options,
                                        const PluginOptions* pl_options,
                                        const ThreadAttributes& attributes,
                                        Mutex& global_mutex) :
    JointTermination(options, pl_options, attributes, global_mutex),
    RingNode(),
    _buffer(nullptr),
    _to_do(),
    _pkt_first(0),
    _pkt_cnt(0),
    _input_end(false),
    _bitrate(0)
{
}

ts::tsp::PluginExecutor::~PluginExecutor()
{
}


//----------------------------------------------------------------------------
// Set the initial state of the buffer. Must be executed in
// synchronous environment, before starting all executor threads.
//----------------------------------------------------------------------------

void ts::tsp::PluginExecutor::initBuffer(PacketBuffer* buffer,
                                         size_t        pkt_first,
                                         size_t        pkt_cnt,
                                         bool          input_end,
                                         bool          aborted,
                                         BitRate       bitrate)
{
    _buffer = buffer;
    _pkt_first = pkt_first;
    _pkt_cnt = pkt_cnt;
    _input_end = input_end;
    _tsp_aborting = aborted;
    _bitrate = bitrate;
    _tsp_bitrate = bitrate;
}


//----------------------------------------------------------------------------
// Signal that the specified number of packets have been processed.
//----------------------------------------------------------------------------

bool ts::tsp::PluginExecutor::passPackets(size_t count, BitRate bitrate, bool input_end, bool aborted)
{
    assert(count <= _pkt_cnt);
    assert(_pkt_first + count <= _buffer->count());

    log(10, u"passPackets(count = %'d, bitrate = %'d, input_end = %s, aborted = %s)", {count, bitrate, input_end, aborted});

    // We access data under the protection of the global mutex.
    Guard lock(_global_mutex);

    // Update our buffer
    _pkt_first = (_pkt_first + count) % _buffer->count();
    _pkt_cnt -= count;

    // Update next processor's buffer.
    PluginExecutor* next = ringNext<PluginExecutor>();
    next->_pkt_cnt += count;
    next->_input_end = next->_input_end || input_end;
    next->_bitrate = bitrate;

    // Wake the next processor when there is some data
    if (count > 0 || input_end) {
        next->_to_do.signal();
    }

    // Force to abort our processor when the next one is aborting.
    // Already done in waitWork() but force immediately.
    // Don't do that if current is output and next is input because
    // there is no propagation of packets from output back to input.
    if (plugin()->type() != OUTPUT_PLUGIN) {
        aborted = aborted || next->_tsp_aborting;
    }

    // Wake the previous processor when we abort
    if (aborted) {
        _tsp_aborting = true; // volatile bool in TSP superclass
        ringPrevious<PluginExecutor>()->_to_do.signal();
    }

    // Return false when the current processor shall stop.
    return !input_end && !aborted;
}


//----------------------------------------------------------------------------
// This method sets the current processor in an abort state.
//----------------------------------------------------------------------------

void ts::tsp::PluginExecutor::setAbort()
{
    Guard lock(_global_mutex);
    _tsp_aborting = true;
    ringPrevious<PluginExecutor>()->_to_do.signal();
}


//----------------------------------------------------------------------------
// Check if the plugin a real time one.
//----------------------------------------------------------------------------

bool ts::tsp::PluginExecutor::isRealTime() const
{
    return plugin() != nullptr && plugin()->isRealTime();
}


//----------------------------------------------------------------------------
// Wait for packets to process or some error condition.
//----------------------------------------------------------------------------

void ts::tsp::PluginExecutor::waitWork(size_t& pkt_first, size_t& pkt_cnt, BitRate& bitrate, bool& input_end, bool& aborted, bool &timeout)
{
    log(10, u"waitWork(...)");

    // We access data under the protection of the global mutex.
    GuardCondition lock(_global_mutex, _to_do);

    PluginExecutor* next = ringNext<PluginExecutor>();
    timeout = false;

    while (_pkt_cnt == 0 && !_input_end && !timeout && !next->_tsp_aborting) {
        // If packet area for this processor is empty, wait for some packet.
        // The mutex is implicitely released, we wait for the condition
        // '_to_do' and, once we get it, implicitely relock the mutex.
        // We loop on this until packets are actually available.
        // If there is a timeout in the packet reception, call the plugin handler.
        timeout = !lock.waitCondition(_tsp_timeout) && !plugin()->handlePacketTimeout();
    }

    pkt_first = _pkt_first;
    pkt_cnt = timeout ? 0 : std::min(_pkt_cnt, _buffer->count() - _pkt_first);
    bitrate = _bitrate;
    input_end = _input_end && pkt_cnt == _pkt_cnt;

    // Force to abort our processor when the next one is aborting.
    // Don't do that if current is output and next is input because
    // there is no propagation of packets from output back to input.
    aborted = plugin()->type() != OUTPUT_PLUGIN && next->_tsp_aborting;

    log(10, u"waitWork(pkt_first = %'d, pkt_cnt = %'d, bitrate = %'d, input_end = %s, aborted = %s, timeout = %s)",
        {pkt_first, pkt_cnt, bitrate, input_end, aborted, timeout});
}
