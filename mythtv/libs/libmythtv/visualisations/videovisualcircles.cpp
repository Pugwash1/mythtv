#include <QPen>
#include "videovisualcircles.h"

VideoVisualCircles::VideoVisualCircles(AudioPlayer *audio, MythRender *render)
  : VideoVisualSpectrum(audio, render)
{
    m_numSamples = 32;
}

void VideoVisualCircles::DrawPriv(MythPainter *painter, QPaintDevice* device)
{
    if (!painter)
        return;

    static const QBrush kNobrush(Qt::NoBrush);
    int red = 0;
    int green = 200;
    QPen pen(QColor(red, green, 0, 255));
    int count = m_scale.range();
    int incr = 200 / count;
    int rad = m_range;
    QRect circ(m_area.x() + m_area.width() / 2, m_area.y() + m_area.height() / 2,
               rad, rad);
    painter->Begin(device);
    for (int i = 0; i < count; i++, rad += m_range, red += incr, green -= incr)
    {
        double mag = qAbs((m_magnitudes[i] + m_magnitudes[i + count]) / 2.0);
        if (mag > 1.0)
        {
            pen.setWidth((int)mag);
            painter->DrawRoundRect(circ, rad, kNobrush, pen, 200);
        }
        circ.adjust(-m_range, -m_range, m_range, m_range);
        pen.setColor(QColor(red, green, 0, 255));
    }
    painter->End();
}

bool VideoVisualCircles::InitialisePriv(void)
{
    m_range = (static_cast<double>(m_area.height()) / 2.0)
        / (m_scale.range() - 10);
    m_scaleFactor = 10.0;
    m_falloff = 1.0;

    LOG(VB_GENERAL, LOG_INFO, DESC +
        QString("Initialised Circles with %1 circles.") .arg(m_scale.range()));
    return true;
}

static class VideoVisualCirclesFactory : public VideoVisualFactory
{
  public:
    const QString &name(void) const override // VideoVisualFactory
    {
        static QString s_name("Circles");
        return s_name;
    }

    VideoVisual *Create(AudioPlayer *audio,
                        MythRender  *render) const override // VideoVisualFactory
    {
        return new VideoVisualCircles(audio, render);
    }

    bool SupportedRenderer(RenderType type) override // VideoVisualFactory
    {
        return (type == kRenderOpenGL);
    }
} VideoVisualCirclesFactory;
