/**
 *  DTVRecorder -- base class for Digital Televison recorders
 *  Copyright 2003-2004 by Brandon Beattie, Doug Larrick,
 *    Jason Hoos, and Daniel Thor Kristjansson
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "atscstreamdata.h"
#include "mpegstreamdata.h"
#include "dvbstreamdata.h"
#include "dtvrecorder.h"
#include "programinfo.h"
#include "mythlogging.h"
#include "mpegtables.h"
#include "ringbuffer.h"
#include "tv_rec.h"
#include "mythsystemevent.h"

#define LOC ((m_tvrec) ? \
    QString("DTVRec[%1]: ").arg(m_tvrec->GetInputId()) : \
    QString("DTVRec(0x%1): ").arg(intptr_t(this),0,16))

const uint DTVRecorder::kMaxKeyFrameDistance = 80;

/** \class DTVRecorder
 *  \brief This is a specialization of RecorderBase used to
 *         handle MPEG-2, MPEG-4, MPEG-4 AVC, DVB and ATSC streams.
 *
 *  \sa DBox2Recorder, DVBRecorder, FirewireRecorder,
        HDHRRecoreder, IPTVRecorder
 */

DTVRecorder::DTVRecorder(TVRec *rec) :
    RecorderBase(rec)
{
    SetPositionMapType(MARK_GOP_BYFRAME);
    m_payload_buffer.reserve(TSPacket::kSize * (50 + 1));

    DTVRecorder::ResetForNewFile();

    memset(m_stream_id,  0, sizeof(m_stream_id));
    memset(m_pid_status, 0, sizeof(m_pid_status));
    memset(m_continuity_counter, 0xff, sizeof(m_continuity_counter));

    m_minimum_recording_quality =
        gCoreContext->GetNumSetting("MinimumRecordingQuality", 95);

    m_containerFormat = formatMPEG2_TS;
}

DTVRecorder::~DTVRecorder(void)
{
    StopRecording();

    DTVRecorder::SetStreamData(nullptr);

    if (m_input_pat)
    {
        delete m_input_pat;
        m_input_pat = nullptr;
    }

    if (m_input_pmt)
    {
        delete m_input_pmt;
        m_input_pmt = nullptr;
    }
}

void DTVRecorder::SetOption(const QString &name, const QString &value)
{
    if (name == "recordingtype")
        m_recording_type = value;
    else
        RecorderBase::SetOption(name, value);
}

/** \fn DTVRecorder::SetOption(const QString&,int)
 *  \brief handles the "wait_for_seqstart" option.
 */
void DTVRecorder::SetOption(const QString &name, int value)
{
    if (name == "wait_for_seqstart")
        m_wait_for_keyframe_option = (value == 1);
    else if (name == "recordmpts")
        m_record_mpts = (value != 0);
    else
        RecorderBase::SetOption(name, value);
}

void DTVRecorder::SetOptionsFromProfile(RecordingProfile *profile,
                                        const QString &videodev,
                                        const QString& /*audiodev*/, const QString& /*vbidev*/)
{
    SetOption("videodevice", videodev);
    DTVRecorder::SetOption("tvformat", gCoreContext->GetSetting("TVFormat"));
    SetStrOption(profile, "recordingtype");
    SetIntOption(profile, "recordmpts");
}

/** \fn DTVRecorder::FinishRecording(void)
 *  \brief Flushes the ringbuffer, and if this is not a live LiveTV
 *         recording saves the position map and filesize.
 */
void DTVRecorder::FinishRecording(void)
{
    if (m_ringBuffer)
        m_ringBuffer->WriterFlush();

    if (m_curRecording)
    {
        SetDuration((int64_t)(m_total_duration * 1000));
        SetTotalFrames(m_frames_written_count);
    }

    RecorderBase::FinishRecording();
}

void DTVRecorder::ResetForNewFile(void)
{
    LOG(VB_RECORD, LOG_INFO, LOC + "ResetForNewFile(void)");
    QMutexLocker locker(&m_positionMapLock);

    // m_seen_psp and m_h264_parser should
    // not be reset here. This will only be called just as
    // we're seeing the first packet of a new keyframe for
    // writing to the new file and anything that makes the
    // recorder think we're waiting on another keyframe will
    // send significant amounts of good data to /dev/null.
    // -- Daniel Kristjansson 2011-02-26

    m_start_code                 = 0xffffffff;
    m_first_keyframe             = -1;
    m_has_written_other_keyframe = false;
    m_last_keyframe_seen         = 0;
    m_last_gop_seen              = 0;
    m_last_seq_seen              = 0;
    m_audio_bytes_remaining      = 0;
    m_video_bytes_remaining      = 0;
    m_other_bytes_remaining      = 0;
    //_recording
    m_error                      = QString();

    m_progressive_sequence       = 0;
    m_repeat_pict                = 0;

    //m_pes_synced
    //m_seen_sps
    m_positionMap.clear();
    m_positionMapDelta.clear();
    m_durationMap.clear();
    m_durationMapDelta.clear();

    locker.unlock();
    DTVRecorder::ClearStatistics();
}

void DTVRecorder::ClearStatistics(void)
{
    RecorderBase::ClearStatistics();

    memset(m_ts_count, 0, sizeof(m_ts_count));
    for (int i = 0; i < 256; ++i)
        m_ts_last[i] = -1LL;
    for (int i = 0; i < 256; ++i)
        m_ts_first[i] = -1LL;
    //m_ts_first_dt -- doesn't need to be cleared only used if m_ts_first>=0
    m_packet_count.fetchAndStoreRelaxed(0);
    m_continuity_error_count.fetchAndStoreRelaxed(0);
    m_frames_seen_count          = 0;
    m_frames_written_count       = 0;
    m_total_duration             = 0;
    m_td_base                    = 0;
    m_td_tick_count              = 0;
    m_td_tick_framerate          = FrameRate(0);
}

// documented in recorderbase.h
void DTVRecorder::Reset(void)
{
    LOG(VB_RECORD, LOG_INFO, LOC + "Reset(void)");
    ResetForNewFile();

    m_start_code = 0xffffffff;

    if (m_curRecording)
    {
        m_curRecording->ClearPositionMap(MARK_GOP_BYFRAME);
        m_curRecording->ClearPositionMap(MARK_DURATION_MS);
    }
}

void DTVRecorder::SetStreamData(MPEGStreamData *data)
{
    if (data == m_stream_data)
        return;

    MPEGStreamData *old_data = m_stream_data;
    m_stream_data = data;
    delete old_data;

    if (m_stream_data)
        InitStreamData();
}

void DTVRecorder::InitStreamData(void)
{
    m_stream_data->AddMPEGSPListener(this);
    m_stream_data->AddMPEGListener(this);

    DVBStreamData *dvb = dynamic_cast<DVBStreamData*>(m_stream_data);
    if (dvb)
        dvb->AddDVBMainListener(this);

    ATSCStreamData *atsc = dynamic_cast<ATSCStreamData*>(m_stream_data);

    if (atsc && atsc->DesiredMinorChannel())
        atsc->SetDesiredChannel(atsc->DesiredMajorChannel(),
                                atsc->DesiredMinorChannel());
    else if (m_stream_data->DesiredProgram() >= 0)
        m_stream_data->SetDesiredProgram(m_stream_data->DesiredProgram());
}

void DTVRecorder::BufferedWrite(const TSPacket &tspacket, bool insert)
{
    if (!insert) // PAT/PMT may need inserted in front of any buffered data
    {
        // delay until first GOP to avoid decoder crash on res change
        if (!m_buffer_packets && m_wait_for_keyframe_option &&
            m_first_keyframe < 0)
            return;

        if (m_curRecording && m_timeOfFirstDataIsSet.testAndSetRelaxed(0,1))
        {
            QMutexLocker locker(&m_statisticsLock);
            m_timeOfFirstData = MythDate::current();
            m_timeOfLatestData = MythDate::current();
            m_timeOfLatestDataTimer.start();
        }

        int val = m_timeOfLatestDataCount.fetchAndAddRelaxed(1);
        int thresh = m_timeOfLatestDataPacketInterval.fetchAndAddRelaxed(0);
        if (val > thresh)
        {
            QMutexLocker locker(&m_statisticsLock);
            uint elapsed = m_timeOfLatestDataTimer.restart();
            int interval = thresh;
            if (elapsed > kTimeOfLatestDataIntervalTarget + 250)
                interval = m_timeOfLatestDataPacketInterval
                           .fetchAndStoreRelaxed(thresh * 4 / 5);
            else if (elapsed + 250 < kTimeOfLatestDataIntervalTarget)
                interval = m_timeOfLatestDataPacketInterval
                           .fetchAndStoreRelaxed(thresh * 9 / 8);

            m_timeOfLatestDataCount.fetchAndStoreRelaxed(1);
            m_timeOfLatestData = MythDate::current();

            LOG(VB_RECORD, LOG_DEBUG, LOC +
                QString("Updating timeOfLatestData elapsed(%1) interval(%2)")
                .arg(elapsed).arg(interval));
        }

        // Do we have to buffer the packet for exact keyframe detection?
        if (m_buffer_packets)
        {
            int idx = m_payload_buffer.size();
            m_payload_buffer.resize(idx + TSPacket::kSize);
            memcpy(&m_payload_buffer[idx], tspacket.data(), TSPacket::kSize);
            return;
        }

        // We are free to write the packet, but if we have buffered packet[s]
        // we have to write them first...
        if (!m_payload_buffer.empty())
        {
            if (m_ringBuffer)
                m_ringBuffer->Write(&m_payload_buffer[0], m_payload_buffer.size());
            m_payload_buffer.clear();
        }
    }

    if (m_ringBuffer && m_ringBuffer->Write(tspacket.data(), TSPacket::kSize) < 0 &&
        m_curRecording && m_curRecording->GetRecordingStatus() != RecStatus::Failing)
    {
        LOG(VB_GENERAL, LOG_INFO, LOC +
            QString("BufferedWrite: Writes are failing, "
                    "setting status to %1")
            .arg(RecStatus::toString(RecStatus::Failing, kSingleRecord)));
        SetRecordingStatus(RecStatus::Failing, __FILE__, __LINE__);
    }
}

enum { kExtractPTS, kExtractDTS };
static int64_t extract_timestamp(
    const uint8_t *bufptr, int bytes_left, int pts_or_dts)
{
    if (bytes_left < 4)
        return -1LL;

    bool has_pts = (bufptr[3] & 0x80) != 0;
    int offset = 5;
    if (((kExtractPTS == pts_or_dts) && !has_pts) || (offset + 5 > bytes_left))
        return -1LL;

    bool has_dts = (bufptr[3] & 0x40) != 0;
    if (kExtractDTS == pts_or_dts)
    {
        if (!has_dts)
            return -1LL;
        offset += has_pts ? 5 : 0;
        if (offset + 5 > bytes_left)
            return -1LL;
    }

    return ((uint64_t(bufptr[offset+0] & 0x0e) << 29) |
            (uint64_t(bufptr[offset+1]       ) << 22) |
            (uint64_t(bufptr[offset+2] & 0xfe) << 14) |
            (uint64_t(bufptr[offset+3]       ) <<  7) |
            (uint64_t(bufptr[offset+4] & 0xfe) >>  1));
}

static QDateTime ts_to_qdatetime(
    uint64_t pts, uint64_t pts_first, QDateTime &pts_first_dt)
{
    if (pts < pts_first)
        pts += 0x1FFFFFFFFLL;
    const QDateTime& dt = pts_first_dt;
    return dt.addMSecs((pts - pts_first)/90);
}

static const FrameRate frameRateMap[16] = {
    FrameRate(0),  FrameRate(24000, 1001), FrameRate(24),
    FrameRate(25), FrameRate(30000, 1001), FrameRate(30),
    FrameRate(50), FrameRate(60000, 1001), FrameRate(60),
    FrameRate(0),  FrameRate(0),           FrameRate(0),
    FrameRate(0),  FrameRate(0),           FrameRate(0),
    FrameRate(0)
};

/** \fn DTVRecorder::FindMPEG2Keyframes(const TSPacket* tspacket)
 *  \brief Locates the keyframes and saves them to the position map.
 *
 *   This searches for three magic integers in the stream.
 *   The picture start code 0x00000100, the GOP code 0x000001B8,
 *   and the sequence start code 0x000001B3. The GOP code is
 *   prefered, but is only required of MPEG1 streams, the
 *   sequence start code is a decent fallback for MPEG2
 *   streams, and if all else fails we just look for the picture
 *   start codes and call every 16th frame a keyframe.
 *
 *   NOTE: This does not only find keyframes but also tracks the
 *         total frames as well. At least a couple times seeking
 *         has been broken by short-circuiting the search once
 *         a keyframe stream id has been found. This may work on
 *         some channels, but will break on others so algorithmic
 *         optimizations should be done with great care.
 *
 *  \code
 *   PES header format
 *   byte 0  byte 1  byte 2  byte 3      [byte 4     byte 5]
 *   0x00    0x00    0x01    PESStreamID  PES packet length
 *  \endcode
 *
 *  \return Returns true if packet[s] should be output.
 */
bool DTVRecorder::FindMPEG2Keyframes(const TSPacket* tspacket)
{
    if (!tspacket->HasPayload()) // no payload to scan
        return m_first_keyframe >= 0;

    if (!m_ringBuffer)
        return m_first_keyframe >= 0;

    // if packet contains start of PES packet, start
    // looking for first byte of MPEG start code (3 bytes 0 0 1)
    // otherwise, pick up search where we left off.
    const bool payloadStart = tspacket->PayloadStart();
    m_start_code = (payloadStart) ? 0xffffffff : m_start_code;

    // Just make these local for efficiency reasons (gcc not so smart..)
    const uint maxKFD = kMaxKeyFrameDistance;
    bool hasFrame     = false;
    bool hasKeyFrame  = false;

    uint aspectRatio = 0;
    uint height = 0;
    uint width = 0;
    FrameRate frameRate(0);

    // Scan for PES header codes; specifically picture_start
    // sequence_start (SEQ) and group_start (GOP).
    //   00 00 01 00: picture_start_code
    //   00 00 01 B8: group_start_code
    //   00 00 01 B3: seq_start_code
    //   00 00 01 B5: ext_start_code
    //   (there are others that we don't care about)
    const uint8_t *bufptr = tspacket->data() + tspacket->AFCOffset();
    const uint8_t *bufend = tspacket->data() + TSPacket::kSize;
    m_repeat_pict = 0;

    while (bufptr < bufend)
    {
        bufptr = avpriv_find_start_code(bufptr, bufend, &m_start_code);
        int bytes_left = bufend - bufptr;
        if ((m_start_code & 0xffffff00) == 0x00000100)
        {
            // At this point we have seen the start code 0 0 1
            // the next byte will be the PES packet stream id.
            const int stream_id = m_start_code & 0x000000ff;
            if (PESStreamID::PictureStartCode == stream_id)
                hasFrame = true;
            else if (PESStreamID::GOPStartCode == stream_id)
            {
                m_last_gop_seen  = m_frames_seen_count;
                hasKeyFrame     = true;
            }
            else if (PESStreamID::SequenceStartCode == stream_id)
            {
                m_last_seq_seen  = m_frames_seen_count;
                hasKeyFrame    |= (m_last_gop_seen + maxKFD)<m_frames_seen_count;

                // Look for aspectRatio changes and store them in the database
                aspectRatio = (bufptr[3] >> 4);

                // Get resolution
                height = ((bufptr[1] & 0xf) << 8) | bufptr[2];
                width = (bufptr[0] <<4) | (bufptr[1]>>4);

                frameRate = frameRateMap[(bufptr[3] & 0x0000000f)];
            }
            else if (PESStreamID::MPEG2ExtensionStartCode == stream_id)
            {
                if (bytes_left >= 1)
                {
                    int ext_type = (bufptr[0] >> 4);
                    switch(ext_type)
                    {
                    case 0x1: /* sequence extension */
                        if (bytes_left >= 6)
                        {
                            m_progressive_sequence = bufptr[1] & (1 << 3);
                        }
                        break;
                    case 0x8: /* picture coding extension */
                        if (bytes_left >= 5)
                        {
                            //int picture_structure = bufptr[2]&3;
                            int top_field_first = bufptr[3] & (1 << 7);
                            int repeat_first_field = bufptr[3] & (1 << 1);
                            int progressive_frame = bufptr[4] & (1 << 7);

                            /* check if we must repeat the frame */
                            m_repeat_pict = 1;
                            if (repeat_first_field)
                            {
                                if (m_progressive_sequence)
                                {
                                    if (top_field_first)
                                        m_repeat_pict = 5;
                                    else
                                        m_repeat_pict = 3;
                                }
                                else if (progressive_frame)
                                {
                                    m_repeat_pict = 2;
                                }
                            }
                            // The m_repeat_pict code above matches
                            // mpegvideo_extract_headers(), but the
                            // code in mpeg_field_start() computes a
                            // value one less, which seems correct.
                            --m_repeat_pict;
                        }
                        break;
                    }
                }
            }
            if ((stream_id >= PESStreamID::MPEGVideoStreamBegin) &&
                (stream_id <= PESStreamID::MPEGVideoStreamEnd))
            {
                int64_t pts = extract_timestamp(
                    bufptr, bytes_left, kExtractPTS);
                int64_t dts = extract_timestamp(
                    bufptr, bytes_left, kExtractPTS);
                HandleTimestamps(stream_id, pts, dts);
                // Detect music choice program (very slow frame rate and audio)
                if (m_first_keyframe < 0
                    &&  m_ts_last[stream_id] - m_ts_first[stream_id] > 3*90000)
                {
                    hasKeyFrame = true;
                    m_music_choice = true;
                    LOG(VB_GENERAL, LOG_INFO, LOC + "Music Choice program detected");
                }
            }
        }
    }

    if (hasFrame && !hasKeyFrame)
    {
        // If we have seen kMaxKeyFrameDistance frames since the
        // last GOP or SEQ stream_id, then pretend this picture
        // is a keyframe. We may get artifacts but at least
        // we will be able to skip frames.
        hasKeyFrame = ((m_frames_seen_count & 0xf) == 0U);
        hasKeyFrame &= (m_last_gop_seen + maxKFD) < m_frames_seen_count;
        hasKeyFrame &= (m_last_seq_seen + maxKFD) < m_frames_seen_count;
    }

    // m_buffer_packets will only be true if a payload start has been seen
    if (hasKeyFrame && (m_buffer_packets || m_first_keyframe >= 0))
    {
        LOG(VB_RECORD, LOG_DEBUG, LOC + QString
            ("Keyframe @ %1 + %2 = %3")
            .arg(m_ringBuffer->GetWritePosition())
            .arg(m_payload_buffer.size())
            .arg(m_ringBuffer->GetWritePosition() + m_payload_buffer.size()));

        m_last_keyframe_seen = m_frames_seen_count;
        HandleKeyframe(0);
    }

    if (hasFrame)
    {
        LOG(VB_RECORD, LOG_DEBUG, LOC + QString
            ("Frame @ %1 + %2 = %3")
            .arg(m_ringBuffer->GetWritePosition())
            .arg(m_payload_buffer.size())
            .arg(m_ringBuffer->GetWritePosition() + m_payload_buffer.size()));

        m_buffer_packets = false;  // We now know if it is a keyframe, or not
        m_frames_seen_count++;
        if (!m_wait_for_keyframe_option || m_first_keyframe >= 0)
            UpdateFramesWritten();
        else
        {
            /* Found a frame that is not a keyframe, and we want to
             * start on a keyframe */
            m_payload_buffer.clear();
        }
    }

    if ((aspectRatio > 0) && (aspectRatio != m_videoAspect))
    {
        m_videoAspect = aspectRatio;
        AspectChange((AspectRatio)aspectRatio, m_frames_written_count);
    }

    if (height && width && (height != m_videoHeight || m_videoWidth != width))
    {
        m_videoHeight = height;
        m_videoWidth = width;
        ResolutionChange(width, height, m_frames_written_count);
    }

    if (frameRate.isNonzero() && frameRate != m_frameRate)
    {
        m_frameRate = frameRate;
        LOG(VB_RECORD, LOG_INFO, LOC +
            QString("FindMPEG2Keyframes: frame rate = %1")
            .arg(frameRate.toDouble() * 1000));
        FrameRateChange(frameRate.toDouble() * 1000, m_frames_written_count);
    }

    return m_first_keyframe >= 0;
}

void DTVRecorder::HandleTimestamps(int stream_id, int64_t pts, int64_t dts)
{
    if (pts < 0)
    {
        m_ts_last[stream_id] = -1;
        return;
    }

    if ((dts < 0) && !m_use_pts)
    {
        m_ts_last[stream_id] = -1;
        m_use_pts = true;
        LOG(VB_RECORD, LOG_DEBUG,
            "Switching from dts tracking to pts tracking." +
            QString("TS count is %1").arg(m_ts_count[stream_id]));
    }

    int64_t ts = dts;
    int64_t gap_threshold = 90000; // 1 second
    if (m_use_pts)
    {
        ts = dts;
        gap_threshold = 2*90000; // two seconds, compensate for GOP ordering
    }

    if (m_music_choice)
        gap_threshold = 8*90000; // music choice uses frames every 6 seconds

    if (m_ts_last[stream_id] >= 0)
    {
        int64_t diff = ts - m_ts_last[stream_id];

        // time jumped back more then 10 seconds, handle it as 33bit overflow
        if (diff < (10 * -90000))
            // MAX_PTS is 33bits all 1
            diff += 0x1ffffffffLL;

        // FIXME why do we handle negative gaps (aka overlap) like a gap?
        if (diff < 0)
            diff = -diff;

        if (diff > gap_threshold && m_first_keyframe >= 0)
        {
            QMutexLocker locker(&m_statisticsLock);

            m_recordingGaps.push_back(
                RecordingGap(
                    ts_to_qdatetime(
                        m_ts_last[stream_id], m_ts_first[stream_id],
                        m_ts_first_dt[stream_id]),
                    ts_to_qdatetime(
                        ts, m_ts_first[stream_id], m_ts_first_dt[stream_id])));
            LOG(VB_RECORD, LOG_DEBUG, LOC + QString("Inserted gap %1 dur %2")
                .arg(m_recordingGaps.back().toString()).arg(diff/90000.0));

            if (m_curRecording && m_curRecording->GetRecordingStatus() != RecStatus::Failing)
            {
                RecordingQuality recq(m_curRecording, m_recordingGaps);
                if (recq.IsDamaged())
                {
                    LOG(VB_GENERAL, LOG_INFO, LOC +
                        QString("HandleTimestamps: too much damage, "
                                "setting status to %1")
                        .arg(RecStatus::toString(RecStatus::Failing, kSingleRecord)));
                    SetRecordingStatus(RecStatus::Failing, __FILE__, __LINE__);
                }
            }
        }
    }

    m_ts_last[stream_id] = ts;

    if (m_ts_count[stream_id] < 30)
    {
        if (!m_ts_count[stream_id])
        {
            m_ts_first[stream_id] = ts;
            m_ts_first_dt[stream_id] = MythDate::current();
        }
        else if (ts < m_ts_first[stream_id])
        {
            m_ts_first[stream_id] = ts;
            m_ts_first_dt[stream_id] = MythDate::current();
        }
    }

    m_ts_count[stream_id]++;
}

void DTVRecorder::UpdateFramesWritten(void)
{
    m_frames_written_count++;
    if (!m_td_tick_framerate.isNonzero())
        m_td_tick_framerate = m_frameRate;
    if (m_td_tick_framerate != m_frameRate)
    {
        m_td_base = m_total_duration;
        m_td_tick_count = 0;
        m_td_tick_framerate = m_frameRate;
    }
    m_td_tick_count += (2 + m_repeat_pict);
    if (m_td_tick_framerate.isNonzero())
    {
        m_total_duration = m_td_base + (int64_t) 500 * m_td_tick_count *
            m_td_tick_framerate.getDen() / (double) m_td_tick_framerate.getNum();
    }

    if (m_frames_written_count < 2000 || m_frames_written_count % 1000 == 0)
        LOG(VB_RECORD, LOG_DEBUG,
            QString("count=%1 m_frameRate=%2 tick_frameRate=%3 "
                    "tick_cnt=%4 tick_base=%5 _total_dur=%6")
            .arg(m_frames_written_count)
            .arg(m_frameRate.toString())
            .arg(m_td_tick_framerate.toString())
            .arg(m_td_tick_count)
            .arg(m_td_base)
            .arg(m_total_duration));
}

bool DTVRecorder::FindAudioKeyframes(const TSPacket* /*tspacket*/)
{
    bool hasKeyFrame = false;
    if (!m_ringBuffer || (GetStreamData()->VideoPIDSingleProgram() <= 0x1fff))
        return hasKeyFrame;

    static const uint64_t msec_per_day = 24 * 60 * 60 * 1000ULL;
    const double frame_interval = (1000.0 / m_video_frame_rate);
    uint64_t elapsed = (uint64_t) max(m_audio_timer.elapsed(), 0);
    uint64_t expected_frame =
        (uint64_t) ((double)elapsed / frame_interval);

    while (m_frames_seen_count > expected_frame + 10000)
        expected_frame += (uint64_t) ((double)msec_per_day / frame_interval);

    if (!m_frames_seen_count || (m_frames_seen_count < expected_frame))
    {
        if (!m_frames_seen_count)
            m_audio_timer.start();

        m_buffer_packets = false;
        m_frames_seen_count++;

        if (1 == (m_frames_seen_count & 0x7))
        {
            m_last_keyframe_seen = m_frames_seen_count;
            HandleKeyframe(m_payload_buffer.size());
            hasKeyFrame = true;
        }

        if (!m_wait_for_keyframe_option || m_first_keyframe>=0)
            UpdateFramesWritten();
    }

    return hasKeyFrame;
}

/// Non-Audio/Video data. For streams which contain no audio/video,
/// write just 1 key-frame at the start.
bool DTVRecorder::FindOtherKeyframes(const TSPacket */*tspacket*/)
{
    if (!m_ringBuffer || (GetStreamData()->VideoPIDSingleProgram() <= 0x1fff))
        return true;

    if (m_has_written_other_keyframe)
        return true;

    LOG(VB_RECORD, LOG_INFO, LOC + "DSMCC - FindOtherKeyframes() - "
            "generating initial key-frame");

    m_frames_seen_count++;
    UpdateFramesWritten();
    m_last_keyframe_seen = m_frames_seen_count;

    HandleKeyframe(m_payload_buffer.size());

    m_has_written_other_keyframe = true;

    return true;
}

/** \fn DTVRecorder::HandleKeyframe(uint64_t)
 *  \brief This save the current frame to the position maps
 *         and handles ringbuffer switching.
 */
void DTVRecorder::HandleKeyframe(int64_t extra)
{
    if (!m_ringBuffer)
        return;

    // Perform ringbuffer switch if needed.
    CheckForRingBufferSwitch();

    uint64_t frameNum = m_frames_written_count;
    if (m_first_keyframe < 0)
    {
        m_first_keyframe = frameNum;
        SendMythSystemRecEvent("REC_STARTED_WRITING", m_curRecording);
    }

    // Add key frame to position map
    m_positionMapLock.lock();
    if (!m_positionMap.contains(frameNum))
    {
        int64_t startpos = m_ringBuffer->GetWritePosition() + extra;

        // Don't put negative offsets into the database, they get munged into
        // MAX_INT64 - offset, which is an exceedingly large number, and
        // certainly not valid.
        if (startpos >= 0)
        {
            m_positionMapDelta[frameNum] = startpos;
            m_positionMap[frameNum]      = startpos;
            m_durationMap[frameNum]      = llround(m_total_duration);
            m_durationMapDelta[frameNum] = llround(m_total_duration);
        }
    }
    m_positionMapLock.unlock();
}

/** \fn DTVRecorder::FindH264Keyframes(const TSPacket*)
 *  \brief This searches the TS packet to identify keyframes.
 *  \param tspacket Pointer the the TS packet data.
 *  \return Returns true if a keyframe has been found.
 */
bool DTVRecorder::FindH264Keyframes(const TSPacket *tspacket)
{
    if (!tspacket->HasPayload()) // no payload to scan
        return m_first_keyframe >= 0;

    if (!m_ringBuffer)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "FindH264Keyframes: No ringbuffer");
        return m_first_keyframe >= 0;
    }

    const bool payloadStart = tspacket->PayloadStart();
    if (payloadStart)
    {
        // reset PES sync state
        m_pes_synced = false;
        m_start_code = 0xffffffff;
    }

    uint aspectRatio = 0;
    uint height = 0;
    uint width = 0;
    FrameRate frameRate(0);

    bool hasFrame = false;
    bool hasKeyFrame = false;

    // scan for PES packets and H.264 NAL units
    uint i = tspacket->AFCOffset();
    for (; i < TSPacket::kSize; ++i)
    {
        // special handling required when a new PES packet begins
        if (payloadStart && !m_pes_synced)
        {
            // bounds check
            if (i + 2 >= TSPacket::kSize)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "PES packet start code may overflow to next TS packet, "
                    "aborting keyframe search");
                break;
            }

            // must find the PES start code
            if (tspacket->data()[i++] != 0x00 ||
                tspacket->data()[i++] != 0x00 ||
                tspacket->data()[i++] != 0x01)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "PES start code not found in TS packet with PUSI set");
                break;
            }

            // bounds check
            if (i + 5 >= TSPacket::kSize)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "PES packet headers overflow to next TS packet, "
                    "aborting keyframe search");
                break;
            }

            // now we need to compute where the PES payload begins
            // skip past the stream_id (+1)
            // the next two bytes are the PES packet length (+2)
            // after that, one byte of PES packet control bits (+1)
            // after that, one byte of PES header flags bits (+1)
            // and finally, one byte for the PES header length
            const unsigned char pes_header_length = tspacket->data()[i + 5];

            // bounds check
            if ((i + 6 + pes_header_length) >= TSPacket::kSize)
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "PES packet headers overflow to next TS packet, "
                    "aborting keyframe search");
                break;
            }

            // we now know where the PES payload is
            // normally, we should have used 6, but use 5 because the for
            // loop will bump i
            i += 5 + pes_header_length;
            m_pes_synced = true;

#if 0
            LOG(VB_RECORD, LOG_DEBUG, LOC + "PES synced");
#endif
            continue;
        }

        // ain't going nowhere if we're not PES synced
        if (!m_pes_synced)
            break;

        // scan for a NAL unit start code

        uint32_t bytes_used = m_h264_parser.addBytes
                              (tspacket->data() + i, TSPacket::kSize - i,
                               m_ringBuffer->GetWritePosition());
        i += (bytes_used - 1);

        if (m_h264_parser.stateChanged())
        {
            if (m_h264_parser.onFrameStart() &&
                m_h264_parser.FieldType() != H264Parser::FIELD_BOTTOM)
            {
                hasKeyFrame = m_h264_parser.onKeyFrameStart();
                hasFrame = true;
                m_seen_sps |= hasKeyFrame;

                width = m_h264_parser.pictureWidth();
                height = m_h264_parser.pictureHeight();
                aspectRatio = m_h264_parser.aspectRatio();
                m_h264_parser.getFrameRate(frameRate);
            }
        }
    } // for (; i < TSPacket::kSize; ++i)

    // If it has been more than 511 frames since the last keyframe,
    // pretend we have one.
    if (hasFrame && !hasKeyFrame &&
        (m_frames_seen_count - m_last_keyframe_seen) > 511)
    {
        hasKeyFrame = true;
        LOG(VB_RECORD, LOG_WARNING, LOC +
            QString("FindH264Keyframes: %1 frames without a keyframe.")
            .arg(m_frames_seen_count - m_last_keyframe_seen));
    }

    // m_buffer_packets will only be true if a payload start has been seen
    if (hasKeyFrame && (m_buffer_packets || m_first_keyframe >= 0))
    {
        LOG(VB_RECORD, LOG_DEBUG, LOC + QString
            ("Keyframe @ %1 + %2 = %3 AU %4")
            .arg(m_ringBuffer->GetWritePosition())
            .arg(m_payload_buffer.size())
            .arg(m_ringBuffer->GetWritePosition() + m_payload_buffer.size())
            .arg(m_h264_parser.keyframeAUstreamOffset()));

        m_last_keyframe_seen = m_frames_seen_count;
        HandleH264Keyframe();
    }

    if (hasFrame)
    {
        LOG(VB_RECORD, LOG_DEBUG, LOC + QString
            ("Frame @ %1 + %2 = %3 AU %4")
            .arg(m_ringBuffer->GetWritePosition())
            .arg(m_payload_buffer.size())
            .arg(m_ringBuffer->GetWritePosition() + m_payload_buffer.size())
            .arg(m_h264_parser.keyframeAUstreamOffset()));

        m_buffer_packets = false;  // We now know if this is a keyframe
        m_frames_seen_count++;
        if (!m_wait_for_keyframe_option || m_first_keyframe >= 0)
            UpdateFramesWritten();
        else
        {
            /* Found a frame that is not a keyframe, and we want to
             * start on a keyframe */
            m_payload_buffer.clear();
        }
    }

    if ((aspectRatio > 0) && (aspectRatio != m_videoAspect))
    {
        m_videoAspect = aspectRatio;
        AspectChange((AspectRatio)aspectRatio, m_frames_written_count);
    }

    if (height && width && (height != m_videoHeight || m_videoWidth != width))
    {
        m_videoHeight = height;
        m_videoWidth = width;
        ResolutionChange(width, height, m_frames_written_count);
    }

    if (frameRate.isNonzero() && frameRate != m_frameRate)
    {
        LOG(VB_RECORD, LOG_INFO, LOC +
            QString("FindH264Keyframes: timescale: %1, tick: %2, framerate: %3")
                      .arg( m_h264_parser.GetTimeScale() )
                      .arg( m_h264_parser.GetUnitsInTick() )
                      .arg( frameRate.toDouble() * 1000 ) );
        m_frameRate = frameRate;
        FrameRateChange(frameRate.toDouble() * 1000, m_frames_written_count);
    }

    return m_seen_sps;
}

/** \fn DTVRecorder::HandleH264Keyframe(void)
 *  \brief This save the current frame to the position maps
 *         and handles ringbuffer switching.
 */
void DTVRecorder::HandleH264Keyframe(void)
{
    // Perform ringbuffer switch if needed.
    CheckForRingBufferSwitch();

    uint64_t startpos;
    uint64_t frameNum = m_frames_written_count;

    if (m_first_keyframe < 0)
    {
        m_first_keyframe = frameNum;
        startpos = 0;
        SendMythSystemRecEvent("REC_STARTED_WRITING", m_curRecording);
    }
    else
        startpos = m_h264_parser.keyframeAUstreamOffset();

    // Add key frame to position map
    m_positionMapLock.lock();
    if (!m_positionMap.contains(frameNum))
    {
        m_positionMapDelta[frameNum] = startpos;
        m_positionMap[frameNum]      = startpos;
        m_durationMap[frameNum]      = llround(m_total_duration);
        m_durationMapDelta[frameNum] = llround(m_total_duration);
    }
    m_positionMapLock.unlock();
}

void DTVRecorder::FindPSKeyFrames(const uint8_t *buffer, uint len)
{
    const uint maxKFD = kMaxKeyFrameDistance;

    const uint8_t *bufstart = buffer;
    const uint8_t *bufptr   = buffer;
    const uint8_t *bufend   = buffer + len;

    uint aspectRatio = 0;
    uint height = 0;
    uint width = 0;
    FrameRate frameRate(0);

    uint skip = std::max(m_audio_bytes_remaining, m_other_bytes_remaining);
    while (bufptr + skip < bufend)
    {
        bool hasFrame     = false;
        bool hasKeyFrame  = false;

        const uint8_t *tmp = bufptr;
        bufptr =
            avpriv_find_start_code(bufptr + skip, bufend, &m_start_code);
        m_audio_bytes_remaining = 0;
        m_other_bytes_remaining = 0;
        m_video_bytes_remaining -= std::min(
            (uint)(bufptr - tmp), m_video_bytes_remaining);

        if ((m_start_code & 0xffffff00) != 0x00000100)
            continue;

        // NOTE: Length may be zero for packets that only contain bytes from
        // video elementary streams in TS packets. 13818-1:2000 2.4.3.7
        int pes_packet_length = -1;
        if ((bufend - bufptr) >= 2)
            pes_packet_length = ((bufptr[0]<<8) | bufptr[1]) + 2 + 6;

        const int stream_id = m_start_code & 0x000000ff;
        if (m_video_bytes_remaining)
        {
            if (PESStreamID::PictureStartCode == stream_id)
            { // pes_packet_length is meaningless
                pes_packet_length = -1;
                if (bufend-bufptr >= 4)
                {
                    uint frmtypei = (bufptr[1]>>3) & 0x7;
                    if ((1 <= frmtypei) && (frmtypei <= 5))
                        hasFrame = true;
                }
                else
                {
                    hasFrame = true;
                }
            }
            else if (PESStreamID::GOPStartCode == stream_id)
            { // pes_packet_length is meaningless
                pes_packet_length = -1;
                m_last_gop_seen  = m_frames_seen_count;
                hasKeyFrame     = true;
            }
            else if (PESStreamID::SequenceStartCode == stream_id)
            { // pes_packet_length is meaningless
                pes_packet_length = -1;
                m_last_seq_seen  = m_frames_seen_count;
                hasKeyFrame    |= (m_last_gop_seen + maxKFD)<m_frames_seen_count;

                // Look for aspectRatio changes and store them in the database
                aspectRatio = (bufptr[3] >> 4);

                // Get resolution
                height = ((bufptr[1] & 0xf) << 8) | bufptr[2];
                width = (bufptr[0] <<4) | (bufptr[1]>>4);

                frameRate = frameRateMap[(bufptr[3] & 0x0000000f)];
            }
        }
        else if (!m_video_bytes_remaining && !m_audio_bytes_remaining)
        {
            if ((stream_id >= PESStreamID::MPEGVideoStreamBegin) &&
                (stream_id <= PESStreamID::MPEGVideoStreamEnd))
            { // ok-dvdinfo
                m_video_bytes_remaining = std::max(0, pes_packet_length);
            }
            else if ((stream_id >= PESStreamID::MPEGAudioStreamBegin) &&
                     (stream_id <= PESStreamID::MPEGAudioStreamEnd))
            { // ok-dvdinfo
                m_audio_bytes_remaining = std::max(0, pes_packet_length);
            }
        }

        if (PESStreamID::PaddingStream == stream_id)
        { // ok-dvdinfo
            m_other_bytes_remaining = std::max(0, pes_packet_length);
        }

        m_start_code = 0xffffffff; // reset start code

        if (hasFrame && !hasKeyFrame)
        {
            // If we have seen kMaxKeyFrameDistance frames since the
            // last GOP or SEQ stream_id, then pretend this picture
            // is a keyframe. We may get artifacts but at least
            // we will be able to skip frames.
            hasKeyFrame = ((m_frames_seen_count & 0xf) == 0U);
            hasKeyFrame &= (m_last_gop_seen + maxKFD) < m_frames_seen_count;
            hasKeyFrame &= (m_last_seq_seen + maxKFD) < m_frames_seen_count;
        }

        if (hasFrame)
        {
            m_frames_seen_count++;
            if (!m_wait_for_keyframe_option || m_first_keyframe >= 0)
                UpdateFramesWritten();
        }

        if (hasKeyFrame)
        {
            m_last_keyframe_seen = m_frames_seen_count;
            HandleKeyframe((int64_t)m_payload_buffer.size() - (bufptr - bufstart));
        }

        if ((aspectRatio > 0) && (aspectRatio != m_videoAspect))
        {
            m_videoAspect = aspectRatio;
            AspectChange((AspectRatio)aspectRatio, m_frames_written_count);
        }

        if (height && width &&
            (height != m_videoHeight || m_videoWidth != width))
        {
            m_videoHeight = height;
            m_videoWidth = width;
            ResolutionChange(width, height, m_frames_written_count);
        }

        if (frameRate.isNonzero() && frameRate != m_frameRate)
        {
            m_frameRate = frameRate;
            LOG(VB_RECORD, LOG_INFO, LOC +
                QString("FindPSKeyFrames: frame rate = %1")
                .arg(frameRate.toDouble() * 1000));
            FrameRateChange(frameRate.toDouble() * 1000, m_frames_written_count);
        }

        if (hasKeyFrame || hasFrame)
        {
            // We are free to write the packet, but if we have
            // buffered packet[s] we have to write them first...
            if (!m_payload_buffer.empty())
            {
                if (m_ringBuffer)
                {
                    m_ringBuffer->Write(
                        &m_payload_buffer[0], m_payload_buffer.size());
                }
                m_payload_buffer.clear();
            }

            if (m_ringBuffer)
                m_ringBuffer->Write(bufstart, (bufptr - bufstart));

            bufstart = bufptr;
        }

        skip = std::max(m_audio_bytes_remaining, m_other_bytes_remaining);
    }

    int bytes_skipped = bufend - bufptr;
    if (bytes_skipped > 0)
    {
        m_audio_bytes_remaining -= std::min(
            (uint)bytes_skipped, m_audio_bytes_remaining);
        m_video_bytes_remaining -= std::min(
            (uint)bytes_skipped, m_video_bytes_remaining);
        m_other_bytes_remaining -= std::min(
            (uint)bytes_skipped, m_other_bytes_remaining);
    }

    uint64_t idx = m_payload_buffer.size();
    uint64_t rem = (bufend - bufstart);
    m_payload_buffer.resize(idx + rem);
    memcpy(&m_payload_buffer[idx], bufstart, rem);
#if 0
    LOG(VB_GENERAL, LOG_DEBUG, LOC +
        QString("idx: %1, rem: %2").arg(idx).arg(rem));
#endif
}

void DTVRecorder::HandlePAT(const ProgramAssociationTable *_pat)
{
    if (!_pat)
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "SetPAT(NULL)");
        return;
    }

    QMutexLocker change_lock(&m_pid_lock);

    int progNum = m_stream_data->DesiredProgram();
    uint pmtpid = _pat->FindPID(progNum);

    if (!pmtpid)
    {
        LOG(VB_RECORD, LOG_ERR, LOC +
            QString("SetPAT(): Ignoring PAT not containing our desired "
                    "program (%1)...").arg(progNum));
        return;
    }

    LOG(VB_RECORD, LOG_INFO, LOC + QString("SetPAT(%1 on 0x%2)")
            .arg(progNum).arg(pmtpid,0,16));

    ProgramAssociationTable *oldpat = m_input_pat;
    m_input_pat = new ProgramAssociationTable(*_pat);
    delete oldpat;

    // Listen for the other PMTs for faster channel switching
    for (uint i = 0; m_input_pat && (i < m_input_pat->ProgramCount()); ++i)
    {
        uint pmt_pid = m_input_pat->ProgramPID(i);
        if (!m_stream_data->IsListeningPID(pmt_pid))
            m_stream_data->AddListeningPID(pmt_pid, kPIDPriorityLow);
    }
}

void DTVRecorder::HandlePMT(uint progNum, const ProgramMapTable *_pmt)
{
    QMutexLocker change_lock(&m_pid_lock);

    LOG(VB_RECORD, LOG_INFO, LOC + QString("SetPMT(%1, %2)").arg(progNum)
        .arg(_pmt == nullptr ? "NULL" : "valid"));


    if ((int)progNum == m_stream_data->DesiredProgram())
    {
        LOG(VB_RECORD, LOG_INFO, LOC + QString("SetPMT(%1)").arg(progNum));
        ProgramMapTable *oldpmt = m_input_pmt;
        m_input_pmt = new ProgramMapTable(*_pmt);

        QString sistandard = GetSIStandard();

        bool has_no_av = true;
        for (uint i = 0; i < m_input_pmt->StreamCount() && has_no_av; ++i)
        {
            has_no_av &= !m_input_pmt->IsVideo(i, sistandard);
            has_no_av &= !m_input_pmt->IsAudio(i, sistandard);
        }
        m_has_no_av = has_no_av;

        SetCAMPMT(m_input_pmt);
        delete oldpmt;
    }
}

void DTVRecorder::HandleSingleProgramPAT(ProgramAssociationTable *pat,
                                         bool insert)
{
    if (!pat)
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "HandleSingleProgramPAT(NULL)");
        return;
    }

    if (!m_ringBuffer)
        return;

    uint next_cc = (pat->tsheader()->ContinuityCounter()+1)&0xf;
    pat->tsheader()->SetContinuityCounter(next_cc);
    pat->GetAsTSPackets(m_scratch, next_cc);

    for (size_t i = 0; i < m_scratch.size(); ++i)
        DTVRecorder::BufferedWrite(m_scratch[i], insert);
}

void DTVRecorder::HandleSingleProgramPMT(ProgramMapTable *pmt, bool insert)
{
    if (!pmt)
    {
        LOG(VB_RECORD, LOG_ERR, LOC + "HandleSingleProgramPMT(NULL)");
        return;
    }

    // We only want to do these checks once per recording
    bool seenVideo = (m_primaryVideoCodec != AV_CODEC_ID_NONE);
    bool seenAudio = (m_primaryAudioCodec != AV_CODEC_ID_NONE);
    uint bestAudioCodec = 0;
    // collect stream types for H.264 (MPEG-4 AVC) keyframe detection
    for (uint i = 0; i < pmt->StreamCount(); ++i)
    {
        // We only care about the first identifiable video stream
        if (!seenVideo && (m_primaryVideoCodec == AV_CODEC_ID_NONE) &&
            StreamID::IsVideo(pmt->StreamType(i)))
        {
            seenVideo = true; // Ignore other video streams
            switch (pmt->StreamType(i))
            {
                case StreamID::MPEG1Video:
                    m_primaryVideoCodec = AV_CODEC_ID_MPEG1VIDEO;
                    break;
                case StreamID::MPEG2Video:
                    m_primaryVideoCodec = AV_CODEC_ID_MPEG2VIDEO;
                    break;
                case StreamID::MPEG4Video:
                    m_primaryVideoCodec = AV_CODEC_ID_MPEG4;
                    break;
                case StreamID::H264Video:
                    m_primaryVideoCodec = AV_CODEC_ID_H264;
                    break;
                case StreamID::H265Video:
                    m_primaryVideoCodec = AV_CODEC_ID_H265;
                    break;
                case StreamID::OpenCableVideo:
                    m_primaryVideoCodec = AV_CODEC_ID_MPEG2VIDEO; // TODO Will it always be MPEG2?
                    break;
                case StreamID::VC1Video:
                    m_primaryVideoCodec = AV_CODEC_ID_VC1;
                    break;
                default:
                    break;
            }

            if (m_primaryVideoCodec != AV_CODEC_ID_NONE)
                VideoCodecChange(m_primaryVideoCodec);
        }

        // We want the 'best' identifiable audio stream, where 'best' is
        // subjective and no-one will likely agree.
        // For now it's the 'best' codec, assuming mpeg stream types range
        // from worst to best, which it does
        if (!seenAudio && StreamID::IsAudio(pmt->StreamType(i)) &&
            pmt->StreamType(i) > bestAudioCodec)
        {
            bestAudioCodec = pmt->StreamType(i);
            switch (pmt->StreamType(i))
            {
                case StreamID::MPEG1Audio:
                    m_primaryAudioCodec = AV_CODEC_ID_MP2; // MPEG-1 Layer 2 (MP2)
                    break;
                case StreamID::MPEG2Audio:
                    m_primaryAudioCodec = AV_CODEC_ID_MP2; // MPEG-2 Part 3 (MP2 Multichannel)
                    break;
                case StreamID::MPEG2AACAudio:
                    m_primaryAudioCodec = AV_CODEC_ID_AAC;
                    break;
                case StreamID::MPEG2AudioAmd1:
                    m_primaryAudioCodec = AV_CODEC_ID_AAC_LATM;
                    break;
                case StreamID::AC3Audio:
                    m_primaryAudioCodec = AV_CODEC_ID_AC3;
                    break;
                case StreamID::EAC3Audio:
                    m_primaryAudioCodec = AV_CODEC_ID_EAC3;
                    break;
                case StreamID::DTSAudio:
                    m_primaryAudioCodec = AV_CODEC_ID_DTS;
                    break;
                default:
                    break;
            }

            if (m_primaryAudioCodec != AV_CODEC_ID_NONE)
                AudioCodecChange(m_primaryAudioCodec);
        }

//         LOG(VB_GENERAL, LOG_DEBUG, QString("Recording(%1): Stream #%2: %3 ")
//             .arg(m_curRecording ? QString::number(m_curRecording->GetRecordingID()) : "")
//             .arg(i)
//             .arg(StreamID::GetDescription(pmt->StreamType(i))));
        m_stream_id[pmt->StreamPID(i)] = pmt->StreamType(i);
    }

    // If the PCRPID is valid and the PCR is not contained
    // in another stream, make sure the PCR stream is not
    // discarded (use PrivSec type as dummy 'valid' value)
    if (pmt->PCRPID() != 0x1fff && pmt->FindPID(pmt->PCRPID()) == -1)
        m_stream_id[pmt->PCRPID()] = StreamID::PrivSec;

    if (!m_ringBuffer)
        return;

    uint next_cc = (pmt->tsheader()->ContinuityCounter()+1)&0xf;
    pmt->tsheader()->SetContinuityCounter(next_cc);
    pmt->GetAsTSPackets(m_scratch, next_cc);

    for (size_t i = 0; i < m_scratch.size(); ++i)
        DTVRecorder::BufferedWrite(m_scratch[i], insert);
}

bool DTVRecorder::ProcessTSPacket(const TSPacket &tspacket)
{
    const uint pid = tspacket.PID();

    if (pid != 0x1fff)
        m_packet_count.fetchAndAddAcquire(1);

    // Check continuity counter
    uint old_cnt = m_continuity_counter[pid];
    if ((pid != 0x1fff) && !CheckCC(pid, tspacket.ContinuityCounter()))
    {
        int v = m_continuity_error_count.fetchAndAddRelaxed(1) + 1;
        double erate = v * 100.0 / m_packet_count.fetchAndAddRelaxed(0);
        LOG(VB_RECORD, LOG_WARNING, LOC +
            QString("PID 0x%1 discontinuity detected ((%2+1)%16!=%3) %4%")
                .arg(pid,0,16).arg(old_cnt,2)
                .arg(tspacket.ContinuityCounter(),2)
                .arg(erate));
    }

    // Only create fake keyframe[s] if there are no audio/video streams
    if (m_input_pmt && m_has_no_av)
    {
        FindOtherKeyframes(&tspacket);
        m_buffer_packets = false;
    }
    else if (m_record_mpts_only)
    {
        /* When recording the full, unfiltered, MPTS, trigger a write
         * every 0.5 seconds.  Since the packets are unfiltered and
         * unprocessed we cannot wait for a keyframe to trigger the
         * writes. */

        static MythTimer timer;

        if (m_frames_seen_count++ == 0)
            timer.start();

        if (timer.elapsed() > 500) // 0.5 seconds
        {
            UpdateFramesWritten();
            m_last_keyframe_seen = m_frames_seen_count;
            HandleKeyframe(m_payload_buffer.size());
            timer.addMSecs(-500);
        }
    }
    else if (m_stream_id[pid] == 0)
    {
        // Ignore this packet if the PID should be stripped
        return true;
    }
    else
    {
        // There are audio/video streams. Only write the packet
        // if audio/video key-frames have been found
        if (m_wait_for_keyframe_option && m_first_keyframe < 0)
            return true;
    }

    BufferedWrite(tspacket);

    return true;
}

bool DTVRecorder::ProcessVideoTSPacket(const TSPacket &tspacket)
{
    if (!m_ringBuffer)
        return true;

    uint streamType = m_stream_id[tspacket.PID()];

    if (tspacket.HasPayload() && tspacket.PayloadStart())
    {
        if (m_buffer_packets && m_first_keyframe >= 0 && !m_payload_buffer.empty())
        {
            // Flush the buffer
            if (m_ringBuffer)
                m_ringBuffer->Write(&m_payload_buffer[0], m_payload_buffer.size());
            m_payload_buffer.clear();
        }

        // buffer packets until we know if this is a keyframe
        m_buffer_packets = true;
    }

    // Check for keyframes and count frames
    if (streamType == StreamID::H264Video)
        FindH264Keyframes(&tspacket);
    else if (streamType != 0)
        FindMPEG2Keyframes(&tspacket);
    else
        LOG(VB_RECORD, LOG_ERR, LOC +
            "ProcessVideoTSPacket: unknown stream type!");

    return ProcessAVTSPacket(tspacket);
}

bool DTVRecorder::ProcessAudioTSPacket(const TSPacket &tspacket)
{
    if (!m_ringBuffer)
        return true;

    if (tspacket.HasPayload() && tspacket.PayloadStart())
    {
        if (m_buffer_packets && m_first_keyframe >= 0 && !m_payload_buffer.empty())
        {
            // Flush the buffer
            if (m_ringBuffer)
                m_ringBuffer->Write(&m_payload_buffer[0], m_payload_buffer.size());
            m_payload_buffer.clear();
        }

        // buffer packets until we know if this is a keyframe
        m_buffer_packets = true;
    }

    FindAudioKeyframes(&tspacket);
    return ProcessAVTSPacket(tspacket);
}

/// Common code for processing either audio or video packets
bool DTVRecorder::ProcessAVTSPacket(const TSPacket &tspacket)
{
    // Sync recording start to first keyframe
    if (m_wait_for_keyframe_option && m_first_keyframe < 0)
    {
        if (m_buffer_packets)
            BufferedWrite(tspacket);
        return true;
    }

    const uint pid = tspacket.PID();

    if (pid != 0x1fff)
        m_packet_count.fetchAndAddAcquire(1);

    // Check continuity counter
    uint old_cnt = m_continuity_counter[pid];
    if ((pid != 0x1fff) && !CheckCC(pid, tspacket.ContinuityCounter()))
    {
        int v = m_continuity_error_count.fetchAndAddRelaxed(1) + 1;
        double erate = v * 100.0 / m_packet_count.fetchAndAddRelaxed(0);
        LOG(VB_RECORD, LOG_WARNING, LOC +
            QString("A/V PID 0x%1 discontinuity detected ((%2+1)%16!=%3) %4%")
                .arg(pid,0,16).arg(old_cnt).arg(tspacket.ContinuityCounter())
                .arg(erate,5,'f',2));
    }

    if (!(m_pid_status[pid] & kPayloadStartSeen))
    {
        m_pid_status[pid] |= kPayloadStartSeen;
        LOG(VB_RECORD, LOG_INFO, LOC +
            QString("PID 0x%1 Found Payload Start").arg(pid,0,16));
    }

    BufferedWrite(tspacket);

    return true;
}

RecordingQuality *DTVRecorder::GetRecordingQuality(const RecordingInfo *r) const
{
    RecordingQuality *recq = RecorderBase::GetRecordingQuality(r);
    recq->AddTSStatistics(
        m_continuity_error_count.fetchAndAddRelaxed(0),
        m_packet_count.fetchAndAddRelaxed(0));
    return recq;
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */
