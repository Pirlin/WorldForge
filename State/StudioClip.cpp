#include "State/StudioClip.h"

#include <QtEndian>
#include <bit>

std::optional<worldforge::MeshClip> readStudioClip(QByteArrayView bytes)
{
    using worldforge::MeshClip;
    constexpr qsizetype header = 20;
    if (bytes.size() < header || !bytes.startsWith("WFM1"))
        return {};
    const auto get = [&](qsizetype offset) { return qFromLittleEndian<quint32>(bytes.data()+offset); };
    const auto frames = get(4), vertices = get(8), name = get(16);
    if (frames < 2 || frames > MeshClip::maxFrames || !vertices || vertices > MeshClip::maxSamples/frames
        || !name || name > 128 || bytes.size() != header+name+qsizetype(frames)*vertices*24)
        return {};
    MeshClip clip;
    clip.name.assign(bytes.data()+header,name);
    clip.framesPerSecond = std::bit_cast<float>(get(12));
    clip.vertexCount = vertices;
    qsizetype offset = header+name;
    clip.samples.resize(std::size_t(frames)*vertices);
    for (auto &vertex : clip.samples)
        for (auto *channels : {&vertex.position,&vertex.normal})
            for (auto &value : *channels) {
                value = std::bit_cast<float>(get(offset));
                offset += 4;
            }
    if (!clip.valid(vertices))
        return {};
    return clip;
}

QByteArray writeStudioClip(const worldforge::MeshClip &clip)
{
    QByteArray bytes("WFM1");
    const auto append = [&bytes](quint32 value) {
        char data[4];
        qToLittleEndian(value,data);
        bytes.append(data,4);
    };
    append(quint32(clip.frameCount()));
    append(quint32(clip.vertexCount));
    append(std::bit_cast<quint32>(clip.framesPerSecond));
    append(quint32(clip.name.size()));
    bytes.append(clip.name.data(),clip.name.size());
    for (const auto &vertex : clip.samples)
        for (const auto *channels : {&vertex.position,&vertex.normal})
            for (const auto value : *channels)
                append(std::bit_cast<quint32>(value));
    return bytes;
}
