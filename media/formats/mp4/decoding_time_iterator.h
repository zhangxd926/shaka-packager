// Copyright 2014 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef MEDIA_FORMATS_MP4_DECODING_TIME_ITERATOR_H_
#define MEDIA_FORMATS_MP4_DECODING_TIME_ITERATOR_H_

#include <vector>

#include "media/formats/mp4/box_definitions.h"

namespace edash_packager {
namespace media {
namespace mp4 {

/// Decoding time to sample box (STTS) iterator used to iterate through the
/// compressed table. This class also provides convenient functions to query
/// total number of samples and the duration from start_sample to end_sample.
class DecodingTimeIterator {
 public:
  /// Create DecodingTimeIterator from decoding time to sample box.
  explicit DecodingTimeIterator(
      const DecodingTimeToSample& decoding_time_to_sample);
  ~DecodingTimeIterator();

  /// Advance to the next sample.
  /// @return true if not past the last sample, false otherwise.
  bool AdvanceSample();

  /// @return true if the iterator is still valid, false if past the last
  ///         sample.
  bool IsValid() const;

  /// @return Sample delta for current sample.
  uint32 sample_delta() const { return iterator_->sample_delta; }

  /// @return Duration from start_sample to end_sample, both 1-based, inclusive.
  uint64 Duration(uint32 start_sample, uint32 end_sample) const;

  /// @return Total number of samples in the table.
  uint32 NumSamples() const;

 private:
  uint32 sample_index_;
  const std::vector<DecodingTime>& decoding_time_table_;
  std::vector<DecodingTime>::const_iterator iterator_;

  DISALLOW_COPY_AND_ASSIGN(DecodingTimeIterator);
};

}  // namespace mp4
}  // namespace media
}  // namespace edash_packager

#endif  // MEDIA_FORMATS_MP4_DECODING_TIME_ITERATOR_H_
