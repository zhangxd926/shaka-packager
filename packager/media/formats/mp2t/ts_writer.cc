// Copyright 2016 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp2t/ts_writer.h"

#include <algorithm>

#include "packager/base/logging.h"
#include "packager/media/base/audio_stream_info.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/stream_info.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/formats/mp2t/ts_packet_writer_util.h"

namespace shaka {
namespace media {
namespace mp2t {

namespace {

// For all the pointer fields in the following PAT and PMTs, they are not really
// part of PAT or PMT but it's there so that TsPacket can point to a memory
// location that starts from pointer field.
const uint8_t kProgramAssociationTableId = 0x00;

// This PAT can be used for both encrypted and clear.
const uint8_t kPat[] = {
    0x00,  // pointer field
    kProgramAssociationTableId,
    0xB0,        // The last 2 '00' assumes that this PAT is not very long.
    0x0D,        // Length of the rest of this array.
    0x00, 0x00,  // Transport stream ID is 0.
    0xC1,        // version number 0, current next indicator 1.
    0x00,        // section number
    0x00,        // last section number
    // program number -> PMT PID mapping.
    0x00, 0x01,  // program number is 1.
    0xE0,        // first 3 bits is reserved.
    ProgramMapTableWriter::kPmtPid,
    // CRC32.
    0xF9, 0x62, 0xF5, 0x8B,
};

const bool kHasPcr = true;
const bool kPayloadUnitStartIndicator = true;

// This is the size of the first few fields in a TS packet, i.e. TS packet size
// without adaptation field or the payload.
const int kTsPacketHeaderSize = 4;
const int kTsPacketSize = 188;
const int kTsPacketMaximumPayloadSize =
    kTsPacketSize - kTsPacketHeaderSize;

const size_t kMaxPesPacketLengthValue = 0xFFFF;

void WritePatToBuffer(const uint8_t* pat,
                      int pat_size,
                      ContinuityCounter* continuity_counter,
                      BufferWriter* writer) {
  const int kPatPid = 0;
  WritePayloadToBufferWriter(pat, pat_size, kPayloadUnitStartIndicator, kPatPid,
                             !kHasPcr, 0, continuity_counter, writer);
}

// The only difference between writing PTS or DTS is the leading bits.
void WritePtsOrDts(uint8_t leading_bits,
                   uint64_t pts_or_dts,
                   BufferWriter* writer) {
  // First byte has 3 MSB of PTS.
  uint8_t first_byte =
      leading_bits << 4 | (((pts_or_dts >> 30) & 0x07) << 1) | 1;
  // Second byte has the next 8 bits of pts.
  uint8_t second_byte = (pts_or_dts >> 22) & 0xFF;
  // Third byte has the next 7 bits of pts followed by a marker bit.
  uint8_t third_byte = (((pts_or_dts >> 15) & 0x7F) << 1) | 1;
  // Fourth byte has the next 8 bits of pts.
  uint8_t fourth_byte = ((pts_or_dts >> 7) & 0xFF);
  // Fifth byte has the last 7 bits of pts followed by a marker bit.
  uint8_t fifth_byte = ((pts_or_dts & 0x7F) << 1) | 1;
  writer->AppendInt(first_byte);
  writer->AppendInt(second_byte);
  writer->AppendInt(third_byte);
  writer->AppendInt(fourth_byte);
  writer->AppendInt(fifth_byte);
}

bool WritePesToFile(const PesPacket& pes,
                    ContinuityCounter* continuity_counter,
                    File* file) {
  // The size of the length field.
  const int kAdaptationFieldLengthSize = 1;
  // The size of the flags field.
  const int kAdaptationFieldHeaderSize = 1;
  const int kPcrFieldSize = 6;
  const int kTsPacketMaxPayloadWithPcr =
      kTsPacketMaximumPayloadSize - kAdaptationFieldLengthSize -
      kAdaptationFieldHeaderSize - kPcrFieldSize;
  const uint64_t pcr_base = pes.has_dts() ? pes.dts() : pes.pts();
  const int pid = ProgramMapTableWriter::kElementaryPid;

  // This writer will hold part of PES packet after PES_packet_length field.
  BufferWriter pes_header_writer;
  // The first bit must be '10' for PES with video or audio stream id. The other
  // flags (bits) don't matter so they are 0.
  pes_header_writer.AppendInt(static_cast<uint8_t>(0x80));
  pes_header_writer.AppendInt(
      static_cast<uint8_t>(static_cast<int>(pes.has_pts()) << 7 |
                           static_cast<int>(pes.has_dts()) << 6
                           // Other fields are all 0.
                           ));
  uint8_t pes_header_data_length = 0;
  if (pes.has_pts())
    pes_header_data_length += 5;
  if (pes.has_dts())
    pes_header_data_length += 5;
  pes_header_writer.AppendInt(pes_header_data_length);

  if (pes.has_pts() && pes.has_dts()) {
    WritePtsOrDts(0x03, pes.pts(), &pes_header_writer);
    WritePtsOrDts(0x01, pes.dts(), &pes_header_writer);
  } else if (pes.has_pts()) {
    WritePtsOrDts(0x02, pes.pts(), &pes_header_writer);
  }

  // Put the first TS packet's payload into a buffer. This contains the PES
  // packet's header.
  BufferWriter first_ts_packet_buffer(kTsPacketSize);
  first_ts_packet_buffer.AppendNBytes(static_cast<uint64_t>(0x000001), 3);
  first_ts_packet_buffer.AppendInt(pes.stream_id());
  const size_t pes_packet_length = pes.data().size() + pes_header_writer.Size();
  first_ts_packet_buffer.AppendInt(static_cast<uint16_t>(
      pes_packet_length > kMaxPesPacketLengthValue ? 0 : pes_packet_length));
  first_ts_packet_buffer.AppendBuffer(pes_header_writer);

  const int available_payload =
      kTsPacketMaxPayloadWithPcr - first_ts_packet_buffer.Size();
  const int bytes_consumed =
      std::min(static_cast<int>(pes.data().size()), available_payload);
  first_ts_packet_buffer.AppendArray(pes.data().data(), bytes_consumed);

  BufferWriter output_writer;
  WritePayloadToBufferWriter(first_ts_packet_buffer.Buffer(),
                             first_ts_packet_buffer.Size(),
                             kPayloadUnitStartIndicator, pid, kHasPcr, pcr_base,
                             continuity_counter, &output_writer);

  const size_t remaining_pes_data_size = pes.data().size() - bytes_consumed;
  if (remaining_pes_data_size > 0) {
    WritePayloadToBufferWriter(pes.data().data() + bytes_consumed,
                               remaining_pes_data_size,
                               !kPayloadUnitStartIndicator, pid, !kHasPcr, 0,
                               continuity_counter, &output_writer);
  }
  return output_writer.WriteToFile(file).ok();
}

}  // namespace

TsWriter::TsWriter() {}
TsWriter::~TsWriter() {}

bool TsWriter::Initialize(const StreamInfo& stream_info) {
  const StreamType stream_type = stream_info.stream_type();
  if (stream_type != StreamType::kStreamVideo &&
      stream_type != StreamType::kStreamAudio) {
    LOG(ERROR) << "TsWriter cannot handle stream type " << stream_type
               << " yet.";
    return false;
  }

  if (stream_info.stream_type() == StreamType::kStreamVideo) {
    const VideoStreamInfo& video_stream_info =
        static_cast<const VideoStreamInfo&>(stream_info);
    if (video_stream_info.codec() != VideoCodec::kCodecH264) {
      LOG(ERROR) << "TsWriter cannot handle video codec "
                 << video_stream_info.codec() << " yet.";
      return false;
    }
    pmt_writer_.reset(new H264ProgramMapTableWriter(&pmt_continuity_counter_));
  } else {
    DCHECK_EQ(stream_type, StreamType::kStreamAudio);
    const AudioStreamInfo& audio_stream_info =
        static_cast<const AudioStreamInfo&>(stream_info);
    if (audio_stream_info.codec() != AudioCodec::kCodecAAC) {
      LOG(ERROR) << "TsWriter cannot handle audio codec "
                 << audio_stream_info.codec() << " yet.";
      return false;
    }
    pmt_writer_.reset(new AacProgramMapTableWriter(
        audio_stream_info.codec_config(), &pmt_continuity_counter_));
  }

  return true;
}

bool TsWriter::NewSegment(const std::string& file_name) {
  if (current_file_) {
    LOG(ERROR) << "File " << current_file_->file_name() << " still open.";
    return false;
  }
  current_file_.reset(File::Open(file_name.c_str(), "w"));
  if (!current_file_) {
    LOG(ERROR) << "Failed to open file " << file_name;
    return false;
  }

  BufferWriter psi;
  WritePatToBuffer(kPat, arraysize(kPat), &pat_continuity_counter_, &psi);
  if (encrypted_) {
    if (!pmt_writer_->EncryptedSegmentPmt(&psi)) {
      return false;
    }
  } else {
    if (!pmt_writer_->ClearSegmentPmt(&psi)) {
      return false;
    }
  }

  if (!psi.WriteToFile(current_file_.get()).ok()) {
    LOG(ERROR) << "Failed to write PSI to file.";
    return false;
  }

  return true;
}

void TsWriter::SignalEncrypted() {
  encrypted_ = true;
}

bool TsWriter::FinalizeSegment() {
  return current_file_.release()->Close();
}

bool TsWriter::AddPesPacket(scoped_ptr<PesPacket> pes_packet) {
  DCHECK(current_file_);
  if (!WritePesToFile(*pes_packet, &elementary_stream_continuity_counter_,
                      current_file_.get())) {
    LOG(ERROR) << "Failed to write pes to file.";
    return false;
  }

  // No need to keep pes_packet around so not passing it anywhere.
  return true;
}

void TsWriter::SetProgramMapTableWriterForTesting(
    scoped_ptr<ProgramMapTableWriter> table_writer) {
  pmt_writer_ = table_writer.Pass();
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
