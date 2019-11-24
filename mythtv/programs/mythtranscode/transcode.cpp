#include <fcntl.h>
#include <cmath>
#include <iostream>

#include <QStringList>
#include <QMap>
#include <QRegExp>
#include <QList>
#include <QWaitCondition>
#include <QMutex>
#include <QMutexLocker>
#include <QtAlgorithms>

#include "mythconfig.h"

#include "transcode.h"
#include "audiooutput.h"
#include "recordingprofile.h"
#include "mythcorecontext.h"
#include "jobqueue.h"
#include "exitcodes.h"
#include "mthreadpool.h"
#include "deletemap.h"
#include "tvremoteutil.h"

#if CONFIG_LIBMP3LAME
#include "NuppelVideoRecorder.h"
#endif
#include "mythplayer.h"
#include "programinfo.h"
#include "mythdbcon.h"
#include "avformatwriter.h"
#include "HLS/httplivestream.h"

#include "videodecodebuffer.h"
#include "cutter.h"
#include "audioreencodebuffer.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
}
#include "mythavutil.h"

#include <unistd.h> // for unlink()

using namespace std;

#define LOC QString("Transcode: ")

Transcode::Transcode(ProgramInfo *pginfo) :
    m_proginfo(pginfo),
    m_recProfile(new RecordingProfile("Transcoders")),
    keyframedist(30),
#if CONFIG_LIBMP3LAME
    nvr(nullptr),
#endif
    ctx(nullptr),
    outRingBuffer(nullptr),
    fifow(nullptr),
    kfa_table(nullptr),
    showprogress(false),
    recorderOptions(""),
    avfMode(false),
    hlsMode(false),                 hlsStreamID(-1),
    hlsDisableAudioOnly(false),
    hlsMaxSegments(0),
    cmdContainer("mpegts"),         cmdAudioCodec("aac"),
    cmdVideoCodec("libx264"),
    cmdWidth(480),                  cmdHeight(0),
    cmdBitrate(600000),             cmdAudioBitrate(64000)
{
}

Transcode::~Transcode()
{
#if CONFIG_LIBMP3LAME
    delete nvr;
#endif
    SetPlayerContext(nullptr);
    delete outRingBuffer;
    delete fifow;
    delete kfa_table;
    delete m_recProfile;
}
void Transcode::ReencoderAddKFA(long curframe, long lastkey, long num_keyframes)
{
    long delta = curframe - lastkey;
    if (delta != 0 && delta != keyframedist)
    {
        struct kfatable_entry kfate {};
        kfate.adjust = keyframedist - delta;
        kfate.keyframe_number = num_keyframes;
        kfa_table->push_back(kfate);
    }
}

bool Transcode::GetProfile(const QString& profileName, const QString& encodingType,
                           int height, int frameRate)
{
    if (profileName.toLower() == "autodetect")
    {
        if (height == 1088)
            height = 1080;

        QString autoProfileName = QObject::tr("Autodetect from %1").arg(height);
        if (frameRate == 25 || frameRate == 30)
            autoProfileName += "i";
        if (frameRate == 50 || frameRate == 60)
            autoProfileName += "p";

        bool result = false;
        LOG(VB_GENERAL, LOG_NOTICE,
            QString("Transcode: Looking for autodetect profile: %1")
                .arg(autoProfileName));
        result = m_recProfile->loadByGroup(autoProfileName, "Transcoders");

        if (!result && encodingType == "MPEG-2")
        {
            result = m_recProfile->loadByGroup("MPEG2", "Transcoders");
            autoProfileName = "MPEG2";
        }
        if (!result && (encodingType == "MPEG-4" || encodingType == "RTjpeg"))
        {
            result = m_recProfile->loadByGroup("RTjpeg/MPEG4",
                                         "Transcoders");
            autoProfileName = "RTjpeg/MPEG4";
        }
        if (!result)
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("Transcode: Couldn't find profile for : %1")
                    .arg(encodingType));

            return false;
        }

        LOG(VB_GENERAL, LOG_NOTICE,
            QString("Transcode: Using autodetect profile: %1")
                .arg(autoProfileName));
    }
    else
    {
        bool isNum = false;
        int profileID = profileName.toInt(&isNum);
        // If a bad profile is specified, there will be trouble
        if (isNum && profileID > 0)
            m_recProfile->loadByID(profileID);
        else if (!m_recProfile->loadByGroup(profileName, "Transcoders"))
        {
            LOG(VB_GENERAL, LOG_ERR, QString("Couldn't find profile #: %1")
                    .arg(profileName));
            return false;
        }
    }
    return true;
}

void Transcode::SetPlayerContext(PlayerContext *player_ctx)
{
    if (player_ctx == ctx)
        return;

    delete ctx;
    ctx = player_ctx;
}

#if CONFIG_LIBMP3LAME
static QString get_str_option(RecordingProfile *profile, const QString &name)
{
    const StandardSetting *setting = profile->byName(name);
    if (setting)
        return setting->getValue();

    LOG(VB_GENERAL, LOG_ERR, LOC +
        QString("get_str_option(...%1): Option not in profile.").arg(name));

    return QString();
}

static int get_int_option(RecordingProfile *profile, const QString &name)
{
    QString ret_str = get_str_option(profile, name);
    if (ret_str.isEmpty())
        return 0;

    bool ok = false;
    int ret_int = ret_str.toInt(&ok);

    if (!ok)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC +
            QString("get_int_option(...%1): Option is not an int.").arg(name));
    }

    return ret_int;
}

static bool get_bool_option(RecordingProfile *profile, const QString &name)
{
    return get_int_option(profile, name) != 0;
}

static void TranscodeWriteText(void *ptr, unsigned char *buf, int len,
                               int timecode, int pagenr)
{
    NuppelVideoRecorder *nvr = (NuppelVideoRecorder *)ptr;
    nvr->WriteText(buf, len, timecode, pagenr);
}
#endif // CONFIG_LIBMP3LAME

int Transcode::TranscodeFile(const QString &inputname,
                             const QString &outputname,
                             const QString &profileName,
                             bool honorCutList, bool framecontrol,
                             int jobID, const QString& fifodir,
                             bool fifo_info, bool cleanCut,
                             frm_dir_map_t &deleteMap,
                             int AudioTrackNo,
                             bool forcefps, bool passthru)

{
    QDateTime curtime = MythDate::current();
    QDateTime statustime = curtime;
    int audioFrame = 0;
    Cutter *cutter = nullptr;
    AVFormatWriter *avfw = nullptr;
    AVFormatWriter *avfw2 = nullptr;
    HTTPLiveStream *hls = nullptr;
    int hlsSegmentSize = 0;
    int hlsSegmentFrames = 0;

#if !CONFIG_LIBMP3LAME
    (void)profileName;
#endif

    if (jobID >= 0)
        JobQueue::ChangeJobComment(jobID, "0% " + QObject::tr("Completed"));

    if (hlsMode)
    {
        avfMode = true;

        if (hlsStreamID != -1)
        {
            hls = new HTTPLiveStream(hlsStreamID);
            hls->UpdateStatus(kHLSStatusStarting);
            hls->UpdateStatusMessage("Transcoding Starting");
            cmdWidth = hls->GetWidth();
            cmdHeight = hls->GetHeight();
            cmdBitrate = hls->GetBitrate();
            cmdAudioBitrate = hls->GetAudioBitrate();
        }
    }

    if (!avfMode)
    {
#if CONFIG_LIBMP3LAME
        nvr = new NuppelVideoRecorder(nullptr, nullptr);
#else
        LOG(VB_GENERAL, LOG_ERR,
            "Not compiled with libmp3lame support");
        return REENCODE_ERROR;
#endif
    }

    // Input setup
    PlayerContext *player_ctx = new PlayerContext(kTranscoderInUseID);
    player_ctx->SetPlayingInfo(m_proginfo);
    RingBuffer *rb = (hls && (hlsStreamID != -1)) ?
        RingBuffer::Create(hls->GetSourceFile(), false, false) :
        RingBuffer::Create(inputname, false, false);
    if (!rb || !rb->GetLastError().isEmpty())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Transcoding aborted, error: '%1'")
            .arg(rb? rb->GetLastError() : ""));
        delete player_ctx;
        delete hls;
        return REENCODE_ERROR;
    }
    player_ctx->SetRingBuffer(rb);
    player_ctx->SetPlayer(new MythPlayer((PlayerFlags)(kVideoIsNull | kNoITV)));
    SetPlayerContext(player_ctx);
    GetPlayer()->SetPlayerInfo(nullptr, nullptr, GetPlayerContext());
    if (m_proginfo->GetRecordingEndTime() > curtime)
    {
        player_ctx->SetRecorder(RemoteGetExistingRecorder(m_proginfo));
        GetPlayer()->SetWatchingRecording(true);
    }

    if (showprogress)
    {
        statustime = statustime.addSecs(5);
    }

    AudioOutput *audioOutput = new AudioReencodeBuffer(FORMAT_NONE, 0,
                                                       passthru);
    AudioReencodeBuffer *arb = ((AudioReencodeBuffer*)audioOutput);
    GetPlayer()->GetAudio()->SetAudioOutput(audioOutput);
    GetPlayer()->SetTranscoding(true);

    if (GetPlayer()->OpenFile() < 0)
    {
        LOG(VB_GENERAL, LOG_ERR, "Transcoding aborted, error opening file.");
        SetPlayerContext(nullptr);
        delete hls;
        return REENCODE_ERROR;
    }

    if (AudioTrackNo > -1)
    {
        LOG(VB_GENERAL, LOG_INFO,
            QString("Set audiotrack number to %1").arg(AudioTrackNo));
        GetPlayer()->GetDecoder()->SetTrack(kTrackTypeAudio, AudioTrackNo);
    }

    long long total_frame_count = GetPlayer()->GetTotalFrameCount();
    long long new_frame_count = total_frame_count;
    if (honorCutList && m_proginfo)
    {
        LOG(VB_GENERAL, LOG_INFO, "Honoring the cutlist while transcoding");

        frm_dir_map_t::const_iterator it;
        QString cutStr;
        long long lastStart = 0;

        if (deleteMap.empty())
            m_proginfo->QueryCutList(deleteMap);

        for (it = deleteMap.begin(); it != deleteMap.end(); ++it)
        {
            if (*it)
            {
                if (!cutStr.isEmpty())
                    cutStr += ",";
                cutStr += QString("%1-").arg((long)it.key());
                lastStart = it.key();
            }
            else
            {
                if (cutStr.isEmpty())
                    cutStr += "0-";
                cutStr += QString("%1").arg((long)it.key());
                new_frame_count -= (it.key() - lastStart);
            }
        }
        if (cutStr.isEmpty())
            cutStr = "Is Empty";
        else if (cutStr.endsWith('-') && (total_frame_count > lastStart))
        {
            new_frame_count -= (total_frame_count - lastStart);
            cutStr += QString("%1").arg(total_frame_count);
        }
        LOG(VB_GENERAL, LOG_INFO, QString("Cutlist        : %1").arg(cutStr));
        LOG(VB_GENERAL, LOG_INFO, QString("Original Length: %1 frames")
                .arg((long)total_frame_count));
        LOG(VB_GENERAL, LOG_INFO, QString("New Length     : %1 frames")
                .arg((long)new_frame_count));

        if ((m_proginfo->QueryIsEditing()) ||
            (JobQueue::IsJobRunning(JOB_COMMFLAG, *m_proginfo)))
        {
            LOG(VB_GENERAL, LOG_INFO, "Transcoding aborted, cutlist changed");
            SetPlayerContext(nullptr);
            delete hls;
            return REENCODE_CUTLIST_CHANGE;
        }
        m_proginfo->ClearMarkupFlag(MARK_UPDATED_CUT);
        curtime = curtime.addSecs(60);
    }

    GetPlayer()->GetAudio()->ReinitAudio();
    QString encodingType = GetPlayer()->GetEncodingType();
    bool copyvideo = false, copyaudio = false;

    QString vidsetting = nullptr, audsetting = nullptr, vidfilters = nullptr;

    QSize buf_size = GetPlayer()->GetVideoBufferSize();
    int video_width = buf_size.width();
    int video_height = buf_size.height();

    if (video_height == 1088) {
       LOG(VB_GENERAL, LOG_NOTICE,
           "Found video height of 1088.  This is unusual and "
           "more than likely the video is actually 1080 so mythtranscode "
           "will treat it as such.");
    }

    DecoderBase* dec = GetPlayer()->GetDecoder();
    float video_aspect = dec ? dec->GetVideoAspect() : 4.0F / 3.0F;
    float video_frame_rate = GetPlayer()->GetFrameRate();
    int newWidth = video_width;
    int newHeight = video_height;
    bool halfFramerate = false;
    bool skippedLastFrame = false;

    kfa_table = new vector<struct kfatable_entry>;

    if (forcefps) {
        // we want the true fps so we do this since thats all we have
        if (video_frame_rate > 30) {
            video_frame_rate =video_frame_rate /2;
        }
    }

    if (avfMode)
    {
        newWidth = cmdWidth;
        newHeight = cmdHeight;

        // Absolutely no purpose is served by scaling video up beyond it's
        // original resolution, quality is degraded, transcoding is
        // slower and in future we may wish to scale bitrate according to
        // resolution, so it would also waste bandwidth (when streaming)
        //
        // This change could be said to apply for all transcoding, but for now
        // we're limiting it to HLS where it's uncontroversial
        if (hlsMode)
        {
//             if (newWidth > video_width)
//                 newWidth = video_width;
            if (newHeight > video_height)
            {
                newHeight = video_height;
                newWidth = 0;
            }
        }

        // TODO: is this necessary?  It got commented out, but may still be
        // needed.
        // int actualHeight = (video_height == 1088 ? 1080 : video_height);

        // If height or width are 0, then we need to calculate them
        if (newHeight == 0 && newWidth > 0)
            newHeight = (int)(1.0F * newWidth / video_aspect);
        else if (newWidth == 0 && newHeight > 0)
            newWidth = (int)(1.0F * newHeight * video_aspect);
        else if (newWidth == 0 && newHeight == 0)
        {
            newHeight = 480;
            newWidth = (int)(1.0F * 480 * video_aspect);
            if (newWidth > 640)
            {
                newWidth = 640;
                newHeight = (int)(1.0F * 640 / video_aspect);
            }
        }

        // make sure dimensions are valid for MPEG codecs
        newHeight = (newHeight + 15) & ~0xF;
        newWidth  = (newWidth  + 15) & ~0xF;

        avfw = new AVFormatWriter();
        if (!avfw)
        {
            LOG(VB_GENERAL, LOG_ERR,
                "Transcoding aborted, error creating AVFormatWriter.");
            SetPlayerContext(nullptr);
            delete hls;
            return REENCODE_ERROR;
        }

        avfw->SetVideoBitrate(cmdBitrate);
        avfw->SetHeight(newHeight);
        avfw->SetWidth(newWidth);
        avfw->SetAspect(video_aspect);
        avfw->SetAudioBitrate(cmdAudioBitrate);
        avfw->SetAudioChannels(arb->m_channels);
        avfw->SetAudioFrameRate(arb->m_eff_audiorate);
        avfw->SetAudioFormat(FORMAT_S16);

        if (hlsMode)
        {

            if (hlsStreamID == -1)
            {
                hls = new HTTPLiveStream(inputname, newWidth, newHeight,
                                         cmdBitrate,
                                         cmdAudioBitrate, hlsMaxSegments,
                                         0, 0);

                hlsStreamID = hls->GetStreamID();
                if (!hls || hlsStreamID == -1)
                {
                    LOG(VB_GENERAL, LOG_ERR, "Unable to create new stream");
                    SetPlayerContext(nullptr);
                    delete avfw;
                    delete avfw2;
                    return REENCODE_ERROR;
                }
            }

            int segmentSize = hls->GetSegmentSize();

            LOG(VB_GENERAL, LOG_NOTICE,
                QString("HLS: Using segment size of %1 seconds")
                    .arg(segmentSize));

            if (!hlsDisableAudioOnly)
            {
                int audioOnlyBitrate = hls->GetAudioOnlyBitrate();

                avfw2 = new AVFormatWriter();
                avfw2->SetContainer("mpegts");
                avfw2->SetAudioCodec("aac");
                avfw2->SetAudioBitrate(audioOnlyBitrate);
                avfw2->SetAudioChannels(arb->m_channels);
                avfw2->SetAudioFrameRate(arb->m_eff_audiorate);
                avfw2->SetAudioFormat(FORMAT_S16);
            }

            avfw->SetContainer("mpegts");
            avfw->SetVideoCodec("libx264");
            avfw->SetAudioCodec("aac");
            hls->UpdateStatus(kHLSStatusStarting);
            hls->UpdateStatusMessage("Transcoding Starting");
            hls->UpdateSizeInfo(newWidth, newHeight, video_width, video_height);

            if (!hls->InitForWrite())
            {
                LOG(VB_GENERAL, LOG_ERR, "hls->InitForWrite() failed");
                SetPlayerContext(nullptr);
                delete hls;
                delete avfw;
                delete avfw2;
                return REENCODE_ERROR;
            }

            if (video_frame_rate > 30)
            {
                halfFramerate = true;
                avfw->SetFramerate(video_frame_rate/2);

                if (avfw2)
                    avfw2->SetFramerate(video_frame_rate/2);

                hlsSegmentSize = (int)(segmentSize * video_frame_rate / 2);
            }
            else
            {
                avfw->SetFramerate(video_frame_rate);

                if (avfw2)
                    avfw2->SetFramerate(video_frame_rate);

                hlsSegmentSize = (int)(segmentSize * video_frame_rate);
            }

            avfw->SetKeyFrameDist(30);
            if (avfw2)
                avfw2->SetKeyFrameDist(30);

            hls->AddSegment();
            avfw->SetFilename(hls->GetCurrentFilename());
            if (avfw2)
                avfw2->SetFilename(hls->GetCurrentFilename(true));
        }
        else
        {
            avfw->SetContainer(cmdContainer);
            avfw->SetVideoCodec(cmdVideoCodec);
            avfw->SetAudioCodec(cmdAudioCodec);
            avfw->SetFilename(outputname);
            avfw->SetFramerate(video_frame_rate);
            avfw->SetKeyFrameDist(30);
        }

        int threads    = gCoreContext->GetNumSetting("HTTPLiveStreamThreads", 2);
        QString preset = gCoreContext->GetSetting("HTTPLiveStreamPreset", "veryfast");
        QString tune   = gCoreContext->GetSetting("HTTPLiveStreamTune", "film");

        LOG(VB_GENERAL, LOG_NOTICE,
            QString("x264 HLS using: %1 threads, '%2' profile and '%3' tune")
                .arg(threads).arg(preset).arg(tune));

        avfw->SetThreadCount(threads);
        avfw->SetEncodingPreset(preset);
        avfw->SetEncodingTune(tune);

        if (avfw2)
            avfw2->SetThreadCount(1);

        if (!avfw->Init())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw->Init() failed");
            SetPlayerContext(nullptr);
            delete hls;
            delete avfw;
            delete avfw2;
            return REENCODE_ERROR;
        }

        if (!avfw->OpenFile())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw->OpenFile() failed");
            SetPlayerContext(nullptr);
            delete hls;
            delete avfw;
            delete avfw2;
            return REENCODE_ERROR;
        }

        if (avfw2 && !avfw2->Init())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw2->Init() failed");
            SetPlayerContext(nullptr);
            delete hls;
            delete avfw;
            delete avfw2;
            return REENCODE_ERROR;
        }

        if (avfw2 && !avfw2->OpenFile())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw2->OpenFile() failed");
            SetPlayerContext(nullptr);
            delete hls;
            delete avfw;
            delete avfw2;
            return REENCODE_ERROR;
        }

        arb->m_audioFrameSize = avfw->GetAudioFrameSize() * arb->m_channels * 2;

        GetPlayer()->SetVideoFilters(
            gCoreContext->GetSetting("HTTPLiveStreamFilters", "yadif=1:-1:1"));
    }
#if CONFIG_LIBMP3LAME 
    else if (fifodir.isEmpty())
    {
        if (!GetProfile(profileName, encodingType, video_height,
                        (int)round(video_frame_rate))) {
            LOG(VB_GENERAL, LOG_ERR, "Transcoding aborted, no profile found.");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        // For overriding settings on the command line
        QMap<QString, QString> recorderOptionsMap;
        if (!recorderOptions.isEmpty())
        {
            QStringList options = recorderOptions
                .split(",", QString::SkipEmptyParts);
            int loop = 0;
            while (loop < options.size())
            {
                QStringList tokens = options[loop].split("=");
                if (tokens.length() < 2)
                {
                    LOG(VB_GENERAL, LOG_ERR, "Transcoding aborted, invalid option settings.");
                    return REENCODE_ERROR;
                }
                recorderOptionsMap[tokens[0]] = tokens[1];

                loop++;
            }
        }

        vidsetting = get_str_option(m_recProfile, "videocodec");
        audsetting = get_str_option(m_recProfile, "audiocodec");
        vidfilters = get_str_option(m_recProfile, "transcodefilters");

        if (encodingType == "MPEG-2" &&
            get_bool_option(m_recProfile, "transcodelossless"))
        {
            LOG(VB_GENERAL, LOG_NOTICE, "Switching to MPEG-2 transcoder.");
            SetPlayerContext(nullptr);
            return REENCODE_MPEG2TRANS;
        }

        // Recorder setup
        if (get_bool_option(m_recProfile, "transcodelossless"))
        {
            vidsetting = encodingType;
            audsetting = "MP3";
        }
        else if (get_bool_option(m_recProfile, "transcoderesize"))
        {
            int actualHeight = (video_height == 1088 ? 1080 : video_height);

            GetPlayer()->SetVideoFilters(vidfilters);
            newWidth = get_int_option(m_recProfile, "width");
            newHeight = get_int_option(m_recProfile, "height");

            // If height or width are 0, then we need to calculate them
            if (newHeight == 0 && newWidth > 0)
                newHeight = (int)(1.0 * newWidth * actualHeight / video_width);
            else if (newWidth == 0 && newHeight > 0)
                newWidth = (int)(1.0 * newHeight * video_width / actualHeight);
            else if (newWidth == 0 && newHeight == 0)
            {
                newHeight = 480;
                newWidth = (int)(1.0 * 480 * video_width / actualHeight);
                if (newWidth > 640)
                {
                    newWidth = 640;
                    newHeight = (int)(1.0 * 640 * actualHeight / video_width);
                }
            }

            if (encodingType.startsWith("mpeg", Qt::CaseInsensitive))
            {
                // make sure dimensions are valid for MPEG codecs
                newHeight = (newHeight + 15) & ~0xF;
                newWidth  = (newWidth  + 15) & ~0xF;
            }

            LOG(VB_GENERAL, LOG_INFO, QString("Resizing from %1x%2 to %3x%4")
                    .arg(video_width).arg(video_height)
                    .arg(newWidth).arg(newHeight));
        }
        else  // lossy and no resize
            GetPlayer()->SetVideoFilters(vidfilters);

        // this is ripped from tv_rec SetupRecording. It'd be nice to merge
        nvr->SetOption("inpixfmt", FMT_YV12);

        nvr->SetOption("width", newWidth);
        nvr->SetOption("height", newHeight);

        nvr->SetOption("tvformat", gCoreContext->GetSetting("TVFormat"));
        nvr->SetOption("vbiformat", gCoreContext->GetSetting("VbiFormat"));

        nvr->SetFrameRate(video_frame_rate);
        nvr->SetVideoAspect(video_aspect);
        nvr->SetTranscoding(true);

        if ((vidsetting == "MPEG-4") ||
            (recorderOptionsMap["videocodec"] == "mpeg4"))
        {
            nvr->SetOption("videocodec", "mpeg4");

            nvr->SetIntOption(m_recProfile, "mpeg4bitrate");
            nvr->SetIntOption(m_recProfile, "scalebitrate");
            nvr->SetIntOption(m_recProfile, "mpeg4maxquality");
            nvr->SetIntOption(m_recProfile, "mpeg4minquality");
            nvr->SetIntOption(m_recProfile, "mpeg4qualdiff");
            nvr->SetIntOption(m_recProfile, "mpeg4optionvhq");
            nvr->SetIntOption(m_recProfile, "mpeg4option4mv");
#ifdef USING_FFMPEG_THREADS
            nvr->SetIntOption(m_recProfile, "encodingthreadcount");
#endif
        }
        else if ((vidsetting == "MPEG-2") ||
                 (recorderOptionsMap["videocodec"] == "mpeg2video"))
        {
            nvr->SetOption("videocodec", "mpeg2video");

            nvr->SetIntOption(m_recProfile, "mpeg2bitrate");
            nvr->SetIntOption(m_recProfile, "scalebitrate");
#ifdef USING_FFMPEG_THREADS
            nvr->SetIntOption(m_recProfile, "encodingthreadcount");
#endif
        }
        else if ((vidsetting == "RTjpeg") ||
                 (recorderOptionsMap["videocodec"] == "rtjpeg"))
        {
            nvr->SetOption("videocodec", "rtjpeg");
            nvr->SetIntOption(m_recProfile, "rtjpegquality");
            nvr->SetIntOption(m_recProfile, "rtjpegchromafilter");
            nvr->SetIntOption(m_recProfile, "rtjpeglumafilter");
        }
        else if (vidsetting.isEmpty())
        {
            LOG(VB_GENERAL, LOG_ERR, "No video information found!");
            LOG(VB_GENERAL, LOG_ERR, "Please ensure that recording profiles "
                                     "for the transcoder are set");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }
        else
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("Unknown video codec: %1").arg(vidsetting));
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        nvr->SetOption("samplerate", arb->m_eff_audiorate);
        if (audsetting == "MP3")
        {
            nvr->SetOption("audiocompression", 1);
            nvr->SetIntOption(m_recProfile, "mp3quality");
            copyaudio = true;
        }
        else if (audsetting == "Uncompressed")
        {
            nvr->SetOption("audiocompression", 0);
        }
        else
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("Unknown audio codec: %1").arg(audsetting));
        }

        nvr->AudioInit(true);

        // For overriding settings on the command line
        if (!recorderOptionsMap.empty())
        {
            QMap<QString, QString>::Iterator it;
            QString key, value;
            for (it = recorderOptionsMap.begin();
                 it != recorderOptionsMap.end(); ++it)
            {
                key   = it.key();
                value = *it;

                LOG(VB_GENERAL, LOG_NOTICE,
                    QString("Forcing Recorder option '%1' to '%2'")
                        .arg(key).arg(value));

                if (value.contains(QRegExp("[^0-9]")))
                    nvr->SetOption(key, value);
                else
                    nvr->SetOption(key, value.toInt());

                if (key == "width")
                    newWidth  = (value.toInt() + 15) & ~0xF;
                else if (key == "height")
                    newHeight = (value.toInt() + 15) & ~0xF;
                else if (key == "videocodec")
                {
                    if (value == "mpeg4")
                        vidsetting = "MPEG-4";
                    else if (value == "mpeg2video")
                        vidsetting = "MPEG-2";
                    else if (value == "rtjpeg")
                        vidsetting = "RTjpeg";
                }
            }
        }

        if ((vidsetting == "MPEG-4") ||
            (vidsetting == "MPEG-2"))
            nvr->SetupAVCodecVideo();
        else if (vidsetting == "RTjpeg")
            nvr->SetupRTjpeg();

        outRingBuffer = RingBuffer::Create(outputname, true, false);
        nvr->SetRingBuffer(outRingBuffer);
        nvr->WriteHeader();
        nvr->StreamAllocate();
    }

    if (vidsetting == encodingType && !framecontrol && !avfMode &&
        fifodir.isEmpty() && honorCutList &&
        video_width == newWidth && video_height == newHeight)
    {
        copyvideo = true;
        LOG(VB_GENERAL, LOG_INFO, "Reencoding video in 'raw' mode");
    }
#endif // CONFIG_LIBMP3LAME

    if (honorCutList && !deleteMap.empty())
    {
        if (cleanCut)
        {
            // Have the player seek only part of the way
            // through a cut, and then use the cutter to
            // discard the rest
            cutter = new Cutter();
            cutter->SetCutList(deleteMap, ctx);
            GetPlayer()->SetCutList(cutter->AdjustedCutList());
        }
        else
        {
            // Have the player apply the cut list
            GetPlayer()->SetCutList(deleteMap);
        }
    }

    GetPlayer()->InitForTranscode(copyaudio, copyvideo);
    if (GetPlayer()->IsErrored())
    {
        LOG(VB_GENERAL, LOG_ERR,
            "Unable to initialize MythPlayer for Transcode");
        SetPlayerContext(nullptr);
        delete hls;
        delete avfw;
        delete avfw2;
        return REENCODE_ERROR;
    }

    VideoFrame frame;
    memset(&frame, 0, sizeof(frame));
    // Do not use padding when compressing to RTjpeg or when in fifomode.
    // The RTjpeg compressor doesn't know how to handle strides different to
    // video width.
    // cppcheck-suppress knownConditionTrueFalse
    bool nonAligned = vidsetting == "RTjpeg" || !fifodir.isEmpty(); 
    bool rescale =
        (video_width != newWidth) || (video_height != newHeight)
        || nonAligned;

    if (rescale)
    {
        size_t newSize = 0;
        if (nonAligned)
        {
            // Set a stride identical to actual width, to ease fifo post-conversion process.
            // 1080i/p video is actually 1088 because of the 16x16 blocks so
            // we have to fudge the output size here.  nuvexport knows how to handle
            // this and as of right now it is the only app that uses the fifo ability.
            newSize = buffersize(FMT_YV12, video_width, video_height == 1080 ? 1088 : video_height, 0 /* aligned */);
        }
        else
        {
            newSize = buffersize(FMT_YV12, newWidth, newHeight);
        }
        unsigned char *newFrame = (unsigned char *)av_malloc(newSize);
        if (!newFrame)
        {
            // OOM
            delete hls;
            return REENCODE_ERROR;
        }
        if (nonAligned)
        {
            // Set a stride identical to actual width, to ease fifo post-conversion process.
            init(&frame, FMT_YV12, newFrame, video_width, video_height, newSize, nullptr, nullptr, -1, -1, 0 /* aligned */);
        }
        else
        {
            // use default stride size.
            init(&frame, FMT_YV12, newFrame, newWidth, newHeight, newSize);
        }
    }

    if (!fifodir.isEmpty())
    {
        AudioPlayer *aplayer = GetPlayer()->GetAudio();
        const char  *audio_codec_name = "unknown";

        switch(aplayer->GetCodec())
        {
            case AV_CODEC_ID_AC3:
                audio_codec_name = "ac3";
                break;
            case AV_CODEC_ID_EAC3:
                audio_codec_name = "eac3";
                break;
            case AV_CODEC_ID_DTS:
                audio_codec_name = "dts";
                break;
            case AV_CODEC_ID_TRUEHD:
                audio_codec_name = "truehd";
                break;
            case AV_CODEC_ID_MP3:
                audio_codec_name = "mp3";
                break;
            case AV_CODEC_ID_MP2:
                audio_codec_name = "mp2";
                break;
            case AV_CODEC_ID_AAC:
                audio_codec_name = "aac";
                break;
            case AV_CODEC_ID_AAC_LATM:
                audio_codec_name = "aac_latm";
                break;
            default:
                audio_codec_name = "unknown";
        }

        if (!arb->m_passthru)
            audio_codec_name = "raw";

        // If cutlist is used then get info on first uncut frame
        if (honorCutList && fifo_info)
        {
            bool is_key = false;
            int did_ff = 0;
            GetPlayer()->TranscodeGetNextFrame(did_ff, is_key, true);

            QSize buf_size2 = GetPlayer()->GetVideoBufferSize();
            video_width = buf_size2.width();
            video_height = buf_size2.height();
            video_aspect = GetPlayer()->GetVideoAspect();
            video_frame_rate = GetPlayer()->GetFrameRate();
        }

        // Display details of the format of the fifo data.
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoWidth %1").arg(video_width));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoHeight %1").arg(video_height));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoAspectRatio %1").arg(video_aspect));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoFrameRate %1").arg(video_frame_rate));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioFormat %1").arg(audio_codec_name));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioChannels %1").arg(arb->m_channels));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioSampleRate %1").arg(arb->m_eff_audiorate));

        if(fifo_info)
        {
            // Request was for just the format of fifo data, not for
            // the actual transcode, so stop here.
            unlink(outputname.toLocal8Bit().constData());
            SetPlayerContext(nullptr);
            if (rescale)
            {
                av_freep(&frame.buf);
            }
            delete hls;
            return REENCODE_OK;
        }

        QString audfifo = fifodir + QString("/audout");
        QString vidfifo = fifodir + QString("/vidout");
        int audio_size = arb->m_eff_audiorate * arb->m_bytes_per_frame;
        // framecontrol is true if we want to enforce fifo sync.
        if (framecontrol)
            LOG(VB_GENERAL, LOG_INFO, "Enforcing sync on fifos");
        fifow = new FIFOWriter(2, framecontrol);

        if (!fifow->FIFOInit(0, QString("video"), vidfifo, frame.size, 50) ||
            !fifow->FIFOInit(1, QString("audio"), audfifo, audio_size, 25))
        {
            LOG(VB_GENERAL, LOG_ERR,
                "Error initializing fifo writer.  Aborting");
            unlink(outputname.toLocal8Bit().constData());
            SetPlayerContext(nullptr);
            if (rescale)
            {
                av_freep(&frame.buf);
            }
            delete hls;
            return REENCODE_ERROR;
        }
        LOG(VB_GENERAL, LOG_INFO,
            QString("Video %1x%2@%3fps Audio rate: %4")
                .arg(video_width).arg(video_height)
                .arg(video_frame_rate)
                .arg(arb->m_eff_audiorate));
        LOG(VB_GENERAL, LOG_INFO, "Created fifos. Waiting for connection.");
    }

#if CONFIG_LIBMP3LAME
    bool forceKeyFrames = (fifow == nullptr) ? framecontrol : false;
    bool writekeyframe = true;
    long lastKeyFrame = 0;
    int num_keyframes = 0;
#endif

    frm_dir_map_t::iterator dm_iter;

    int did_ff = 0;

    long curFrameNum = 0;
    frame.frameNumber = 1;
    long totalAudio = 0;
    int dropvideo = 0;
    // timecode of the last read video frame in input time
    long long lasttimecode = 0;
    // timecode of the last write video frame in input or output time
    long long lastWrittenTime = 0;
    // delta between the same video frame in input and output due to applying the cut list
    long long timecodeOffset = 0;

    float rateTimeConv = arb->m_eff_audiorate / 1000.0F;
    float vidFrameTime = 1000.0F / video_frame_rate;
    int wait_recover = 0;
    VideoOutput *videoOutput = GetPlayer()->GetVideoOutput();
    bool is_key = false;
    bool first_loop = true;
    AVFrame imageIn, imageOut;
    struct SwsContext  *scontext = nullptr;

    if (fifow)
        LOG(VB_GENERAL, LOG_INFO, "Dumping Video and Audio data to fifos");
    else if (copyaudio)
        LOG(VB_GENERAL, LOG_INFO, "Copying Audio while transcoding Video");
    else if (hlsMode)
        LOG(VB_GENERAL, LOG_INFO, "Transcoding for HTTP Live Streaming");
    else if (avfMode)
        LOG(VB_GENERAL, LOG_INFO, "Transcoding to libavformat container");
    else
        LOG(VB_GENERAL, LOG_INFO, "Transcoding Video and Audio");

    VideoDecodeBuffer *videoBuffer =
        new VideoDecodeBuffer(GetPlayer(), videoOutput, honorCutList);
    MThreadPool::globalInstance()->start(videoBuffer, "VideoDecodeBuffer");

    QTime flagTime;
    flagTime.start();

    if (cutter)
        cutter->Activate(vidFrameTime * rateTimeConv, total_frame_count);

    bool stopSignalled = false;
    VideoFrame *lastDecode = nullptr;

    if (hls)
    {
        hls->UpdateStatus(kHLSStatusRunning);
        hls->UpdateStatusMessage("Transcoding");
    }

    while ((!stopSignalled) &&
           (lastDecode = videoBuffer->GetFrame(did_ff, is_key)))
    {
        if (first_loop)
        {
            copyaudio = GetPlayer()->GetRawAudioState();
            first_loop = false;
        }

        float new_aspect = lastDecode->aspect;

        if (cutter)
            cutter->NewFrame(lastDecode->frameNumber);

// frame timecode is on input time base
        frame.timecode = lastDecode->timecode;

        // if the timecode jumps backwards just use the last frame's timecode plus the duration of a frame
        if (frame.timecode < lasttimecode)
            frame.timecode = (long long)(lasttimecode + vidFrameTime);

        if (fifow)
        {
            AVPictureFill(&imageIn, lastDecode);
            AVPictureFill(&imageOut, &frame);

            scontext = sws_getCachedContext(scontext,
                           lastDecode->width, lastDecode->height, FrameTypeToPixelFormat(lastDecode->codec),
                           frame.width, frame.height, FrameTypeToPixelFormat(frame.codec),
                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            // Typically, wee aren't rescaling per say, we're just correcting the stride set by the decoder.
            // However, it allows to properly handle recordings that see their resolution change half-way.
            sws_scale(scontext, imageIn.data, imageIn.linesize, 0,
                      lastDecode->height, imageOut.data, imageOut.linesize);

            totalAudio += arb->GetSamples(frame.timecode);
            int audbufTime = (int)(totalAudio / rateTimeConv);
            int auddelta = frame.timecode - audbufTime;
            int vidTime = lroundf(curFrameNum * vidFrameTime);
            int viddelta = frame.timecode - vidTime;
            int delta = viddelta - auddelta;
            int absdelta = delta < 0 ? -delta : delta;
            if (absdelta < 500 && absdelta >= vidFrameTime)
            {
               QString msg = QString("Audio is %1ms %2 video at # %3: "
                                     "auddelta=%4, viddelta=%5")
                   .arg(absdelta)
                   .arg(((delta > 0) ? "ahead of" : "behind"))
                   .arg((int)curFrameNum)
                   .arg(auddelta)
                   .arg(viddelta);
                LOG(VB_GENERAL, LOG_INFO, msg);
                dropvideo = (delta > 0) ? 1 : -1;
                wait_recover = 0;
            }
            else if (delta >= 500 && delta < 10000)
            {
                if (wait_recover == 0)
                {
                    dropvideo = 5;
                    wait_recover = 6;
                }
                else if (wait_recover == 1)
                {
                    // Video is badly lagging.  Try to catch up.
                    int count = 0;
                    while (delta > vidFrameTime)
                    {
                        if (!cutter || !cutter->InhibitDummyFrame())
                            fifow->FIFOWrite(0, frame.buf, frame.size);

                        count++;
                        delta -= (int)vidFrameTime;
                    }
                    QString msg = QString("Added %1 blank video frames")
                                  .arg(count);
                    LOG(VB_GENERAL, LOG_INFO, msg);
                    curFrameNum += count;
                    dropvideo = 0;
                    wait_recover = 0;
                }
                else
                    wait_recover--;
            }
            else
            {
                dropvideo = 0;
                wait_recover = 0;
            }

#if 0
            int buflen = (int)(arb->audiobuffer_len / rateTimeConv);
            LOG(VB_GENERAL, LOG_DEBUG,
                QString("%1: video time: %2 audio time: %3 "
                        "buf: %4 exp: %5 delta: %6")
                    .arg(curFrameNum) .arg(frame.timecode)
                    .arg(arb->last_audiotime) .arg(buflen) .arg(audbufTime)
                    .arg(delta));
#endif
            AudioBuffer *ab = nullptr;
            while ((ab = arb->GetData(frame.timecode)) != nullptr)
            {
                if (!cutter ||
                    !cutter->InhibitUseAudioFrames(ab->m_frames, &totalAudio))
                    fifow->FIFOWrite(1, ab->data(), ab->size());

                delete ab;
            }

            if (dropvideo < 0)
            {
                if (cutter && cutter->InhibitDropFrame())
                    fifow->FIFOWrite(0, frame.buf, frame.size);

                LOG(VB_GENERAL, LOG_INFO, "Dropping video frame");
                dropvideo++;
                curFrameNum--;
            }
            else
            {
                if (!cutter || !cutter->InhibitUseVideoFrame())
                    fifow->FIFOWrite(0, frame.buf, frame.size);

                if (dropvideo)
                {
                    if (!cutter || !cutter->InhibitDummyFrame())
                        fifow->FIFOWrite(0, frame.buf, frame.size);

                    curFrameNum++;
                    dropvideo--;
                }
            }
            videoOutput->DoneDisplayingFrame(lastDecode);
            GetPlayer()->GetCC608Reader()->FlushTxtBuffers();
            lasttimecode = frame.timecode;
        }
        else if (copyaudio)
        {
#if CONFIG_LIBMP3LAME
            // Encoding from NuppelVideo to NuppelVideo with MP3 audio
            // So let's not decode/reencode audio
            if (!GetPlayer()->GetRawAudioState())
            {
                // The Raw state changed during decode.  This is not good
                LOG(VB_GENERAL, LOG_ERR, "Transcoding aborted, MythPlayer "
                                         "is not in raw audio mode.");

                unlink(outputname.toLocal8Bit().constData());
                if (rescale)
                {
                    av_freep(&frame.buf);
                }
                SetPlayerContext(nullptr);
                if (videoBuffer)
                    videoBuffer->stop();
                if (hls)
                {
                    hls->UpdateStatus(kHLSStatusErrored);
                    hls->UpdateStatusMessage("Transcoding Errored");
                    delete hls;
                }
                return REENCODE_ERROR;
            }

            if (forceKeyFrames)
                writekeyframe = true;
            else
            {
                writekeyframe = is_key;
                if (writekeyframe)
                {
                    // Currently, we don't create new sync frames,
                    // (though we do create new 'I' frames), so we mark
                    // the key-frames before deciding whether we need a
                    // new 'I' frame.

                    //need to correct the frame# and timecode here
                    // Question:  Is it necessary to change the timecodes?
                    long sync_offset =
                        GetPlayer()->UpdateStoredFrameNum(curFrameNum);
                    nvr->UpdateSeekTable(num_keyframes, sync_offset);
                    ReencoderAddKFA(curFrameNum, lastKeyFrame, num_keyframes);
                    num_keyframes++;
                    lastKeyFrame = curFrameNum;

                    if (did_ff)
                        did_ff = 0;
                }
            }

            if (did_ff == 1)
            {
                timecodeOffset +=
                    (frame.timecode - lasttimecode - (int)vidFrameTime);
            }
            lasttimecode = frame.timecode;
// from here on the timecode is on the output time base
            frame.timecode -= timecodeOffset;

            if (!GetPlayer()->WriteStoredData(
                    outRingBuffer, (did_ff == 0), timecodeOffset))
            {
                if (video_aspect != new_aspect)
                {
                    video_aspect = new_aspect;
                    nvr->SetNewVideoParams(video_aspect);
                }

                QSize buf_size3 = GetPlayer()->GetVideoBufferSize();

                if (video_width != buf_size3.width() ||
                    video_height != buf_size3.height())
                {
                    video_width = buf_size3.width();
                    video_height = buf_size3.height();

                    LOG(VB_GENERAL, LOG_INFO,
                        QString("Resizing from %1x%2 to %3x%4")
                            .arg(video_width).arg(video_height)
                            .arg(newWidth).arg(newHeight));

                }

                if (did_ff == 1)
                {
                  // Create a new 'I' frame if we just processed a cut.
                  did_ff = 2;
                  writekeyframe = true;
                }

                if (rescale)
                {
                    AVPictureFill(&imageIn, lastDecode);
                    AVPictureFill(&imageOut, &frame);

                    int bottomBand = (lastDecode->height == 1088) ? 8 : 0;
                    scontext = sws_getCachedContext(scontext,
                                   lastDecode->width, lastDecode->height, FrameTypeToPixelFormat(lastDecode->codec),
                                   frame.width, frame.height, FrameTypeToPixelFormat(frame.codec),
                                   SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

                    sws_scale(scontext, imageIn.data, imageIn.linesize, 0,
                              lastDecode->height - bottomBand,
                              imageOut.data, imageOut.linesize);
                }

                nvr->WriteVideo(rescale ? &frame : lastDecode, true, writekeyframe);
            }
            GetPlayer()->GetCC608Reader()->FlushTxtBuffers();
#else
        LOG(VB_GENERAL, LOG_ERR,
            "Not compiled with libmp3lame support. Should never get here");
        return REENCODE_ERROR;
#endif // CONFIG_LIBMP3LAME
        }
        else
        {
            if (did_ff == 1)
            {
                did_ff = 2;
                timecodeOffset +=
                    (frame.timecode - lasttimecode - (int)vidFrameTime);
            }

            if (video_aspect != new_aspect)
            {
                video_aspect = new_aspect;
#if CONFIG_LIBMP3LAME
                if (nvr)
                    nvr->SetNewVideoParams(video_aspect);
#endif
            }


            QSize buf_size4 = GetPlayer()->GetVideoBufferSize();

            if (video_width != buf_size4.width() ||
                video_height != buf_size4.height())
            {
                video_width = buf_size4.width();
                video_height = buf_size4.height();

                LOG(VB_GENERAL, LOG_INFO,
                    QString("Resizing from %1x%2 to %3x%4")
                        .arg(video_width).arg(video_height)
                        .arg(newWidth).arg(newHeight));
            }

            if (rescale)
            {
                AVPictureFill(&imageIn, lastDecode);
                AVPictureFill(&imageOut, &frame);

                lastDecode->height = (lastDecode->height == 1088) ? 1080 : lastDecode->height;
                scontext = sws_getCachedContext(scontext,
                               lastDecode->width, lastDecode->height, FrameTypeToPixelFormat(lastDecode->codec),
                               frame.width, frame.height, FrameTypeToPixelFormat(frame.codec),
                               SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

                sws_scale(scontext, imageIn.data, imageIn.linesize, 0,
                          lastDecode->height,
                          imageOut.data, imageOut.linesize);
            }

            // audio is fully decoded, so we need to reencode it
            AudioBuffer *ab = nullptr;
            while ((ab = arb->GetData(lastWrittenTime)) != nullptr)
            {
                unsigned char *buf = (unsigned char *)ab->data();
                if (avfMode)
                {
                    if (did_ff != 1)
                    {
                        long long tc = ab->m_time - timecodeOffset;
                        avfw->WriteAudioFrame(buf, audioFrame, tc);

                        if (avfw2)
                        {
                            if ((avfw2->GetTimecodeOffset() == -1) &&
                                (avfw->GetTimecodeOffset() != -1))
                            {
                                avfw2->SetTimecodeOffset(
                                    avfw->GetTimecodeOffset());
                            }

                            tc = ab->m_time - timecodeOffset;
                            avfw2->WriteAudioFrame(buf, audioFrame, tc);
                        }

                        ++audioFrame;
                    }
                }
#if CONFIG_LIBMP3LAME
                else
                {
                    nvr->SetOption("audioframesize", ab->size());
                    nvr->WriteAudio(buf, audioFrame++,
                                    ab->m_time - timecodeOffset);
                    if (nvr->IsErrored())
                    {
                        LOG(VB_GENERAL, LOG_ERR,
                            "Transcode: Encountered irrecoverable error in "
                            "NVR::WriteAudio");

                        if (rescale)
                        {
                            av_freep(&frame.buf);
                        }
                        SetPlayerContext(nullptr);
                        if (videoBuffer)
                            videoBuffer->stop();
                        delete ab;
                        delete hls; // HLS isn't actually going to be running here
                        return REENCODE_ERROR;
                    }
                }
#endif
                delete ab;
            }

            if (!avfMode)
            {
#if CONFIG_LIBMP3LAME
                GetPlayer()->GetCC608Reader()->
                    TranscodeWriteText(&TranscodeWriteText, (void *)(nvr));
#else
                LOG(VB_GENERAL, LOG_ERR,
                    "Not compiled with libmp3lame support");
                return REENCODE_ERROR;
#endif
            }
            lasttimecode = frame.timecode;
            frame.timecode -= timecodeOffset;

            if (avfMode)
            {
                if (halfFramerate && !skippedLastFrame)
                {
                    skippedLastFrame = true;
                }
                else
                {
                    skippedLastFrame = false;

                    if ((hls) &&
                        (avfw->GetFramesWritten()) &&
                        (hlsSegmentFrames > hlsSegmentSize) &&
                        (avfw->NextFrameIsKeyFrame()))
                    {
                        hls->AddSegment();
                        avfw->ReOpen(hls->GetCurrentFilename());

                        if (avfw2)
                            avfw2->ReOpen(hls->GetCurrentFilename(true));

                        hlsSegmentFrames = 0;
                    }

                    if (avfw->WriteVideoFrame(rescale ? &frame : lastDecode) > 0)
                    {
                        lastWrittenTime = frame.timecode + timecodeOffset;
                        if (hls)
                            ++hlsSegmentFrames;
                    }

                }
            }
#if CONFIG_LIBMP3LAME
            else
            {
                if (forceKeyFrames)
                    nvr->WriteVideo(rescale ? &frame : lastDecode, true, true);
                else
                    nvr->WriteVideo(rescale ? &frame : lastDecode);
                lastWrittenTime = frame.timecode + timecodeOffset;
            }
#endif
        }
        if (MythDate::current() > statustime)
        {
            if (showprogress)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("Processed: %1 of %2 frames(%3 seconds)").
                        arg(curFrameNum).arg((long)total_frame_count).
                        arg((long)(curFrameNum / video_frame_rate)));
            }

            if (hls && hls->CheckStop())
            {
                hls->UpdateStatus(kHLSStatusStopping);
                stopSignalled = true;
            }

            statustime = MythDate::current().addSecs(5);
        }
        if (MythDate::current() > curtime)
        {
            if (honorCutList && m_proginfo && !avfMode &&
                m_proginfo->QueryMarkupFlag(MARK_UPDATED_CUT))
            {
                LOG(VB_GENERAL, LOG_NOTICE,
                    "Transcoding aborted, cutlist updated");

                unlink(outputname.toLocal8Bit().constData());
                if (rescale)
                {
                    av_freep(&frame.buf);
                }
                SetPlayerContext(nullptr);
                if (videoBuffer)
                    videoBuffer->stop();
                return REENCODE_CUTLIST_CHANGE;
            }

            if ((jobID >= 0) || (VERBOSE_LEVEL_CHECK(VB_GENERAL, LOG_INFO)))
            {
                if (JobQueue::GetJobCmd(jobID) == JOB_STOP)
                {
                    LOG(VB_GENERAL, LOG_NOTICE,
                        "Transcoding STOPped by JobQueue");

                    unlink(outputname.toLocal8Bit().constData());
                    if (rescale)
                    {
                        av_freep(&frame.buf);
                    }
                    SetPlayerContext(nullptr);
                    if (videoBuffer)
                        videoBuffer->stop();
                    if (hls)
                    {
                        hls->UpdateStatus(kHLSStatusStopped);
                        hls->UpdateStatusMessage("Transcoding Stopped");
                        delete hls;
                    }
                    return REENCODE_STOPPED;
                }

                float flagFPS = 0.0;
                float elapsed = flagTime.elapsed() / 1000.0;
                if (elapsed)
                    flagFPS = curFrameNum / elapsed;

                total_frame_count = GetPlayer()->GetCurrentFrameCount();
                int percentage = curFrameNum * 100 / total_frame_count;

                if (hls)
                    hls->UpdatePercentComplete(percentage);

                if (jobID >= 0)
                    JobQueue::ChangeJobComment(jobID,
                              QObject::tr("%1% Completed @ %2 fps.")
                                          .arg(percentage).arg(flagFPS));
                else
                    LOG(VB_GENERAL, LOG_INFO,
                        QString("mythtranscode: %1% Completed @ %2 fps.")
                            .arg(percentage).arg(flagFPS));

            }
            curtime = MythDate::current().addSecs(20);
        }

        curFrameNum++;
        frame.frameNumber = 1 + (curFrameNum << 1);

        GetPlayer()->DiscardVideoFrame(lastDecode);
    }

    sws_freeContext(scontext);

    if (!fifow)
    {
        if (avfw)
            avfw->CloseFile();

        if (avfw2)
            avfw2->CloseFile();

        if (!avfMode && m_proginfo)
        {
            m_proginfo->ClearPositionMap(MARK_KEYFRAME);
            m_proginfo->ClearPositionMap(MARK_GOP_START);
            m_proginfo->ClearPositionMap(MARK_GOP_BYFRAME);
            m_proginfo->ClearPositionMap(MARK_DURATION_MS);
        }

#if CONFIG_LIBMP3LAME
        if (nvr)
        {
            nvr->WriteSeekTable();
            if (!kfa_table->empty())
                nvr->WriteKeyFrameAdjustTable(*kfa_table);
        }
#endif // CONFIG_LIBMP3LAME
    } else {
        fifow->FIFODrain();
    }

    delete cutter;
    delete avfw;
    delete avfw2;

    if (hls)
    {
        if (!stopSignalled)
        {
            hls->UpdateStatus(kHLSStatusCompleted);
            hls->UpdateStatusMessage("Transcoding Completed");
            hls->UpdatePercentComplete(100);
        }
        else
        {
            hls->UpdateStatus(kHLSStatusStopped);
            hls->UpdateStatusMessage("Transcoding Stopped");
        }
        delete hls;
    }

    if (videoBuffer)
    {
        videoBuffer->stop();
    }

    if (rescale)
    {
        av_freep(&frame.buf);
    }
    SetPlayerContext(nullptr);

    return REENCODE_OK;
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */

