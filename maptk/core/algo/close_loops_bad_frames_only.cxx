/*ckwg +5
 * Copyright 2014 by Kitware, Inc. All Rights Reserved. Please refer to
 * KITWARE_LICENSE.TXT for licensing information, or contact General Counsel,
 * Kitware, Inc., 28 Corporate Drive, Clifton Park, NY 12065.
 */

/**
 * \file
 * \brief Implementation of \link maptk::algo::close_loops_bad_frames_only
 *        close_loops_bad_frames_only \endlink
 */

#include <maptk/core/algo/close_loops_bad_frames_only.h>
#include <maptk/core/algo/algorithm.txx>
#include <maptk/core/exceptions/algorithm.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/algorithm/string/join.hpp>


namespace maptk
{

namespace algo
{


/// Default Constructor
close_loops_bad_frames_only
::close_loops_bad_frames_only()
: bf_detection_enabled_(true),
  bf_detection_percent_match_req_(0.2),
  bf_detection_new_shot_length_(2),
  bf_detection_max_search_length_(5)
{
}


/// Copy Constructor
close_loops_bad_frames_only
::close_loops_bad_frames_only(const close_loops_bad_frames_only& other)
: bf_detection_enabled_(other.bf_detection_enabled_),
  bf_detection_percent_match_req_(other.bf_detection_percent_match_req_),
  bf_detection_new_shot_length_(other.bf_detection_new_shot_length_),
  bf_detection_max_search_length_(other.bf_detection_max_search_length_)
{
}


/// Get this alg's \link maptk::config_block configuration block \endlink
config_block_sptr
close_loops_bad_frames_only
::get_configuration() const
{
  // get base config from base class
  config_block_sptr config = algorithm::get_configuration();

  // Sub-algorithm implementation name + sub_config block
  // - Feature Matcher algorithm
  match_features::get_nested_algo_configuration("feature_matcher", config, matcher_);

  // Bad frame detection parameters
  config->set_value("bf_detection_enabled", bf_detection_enabled_,
                    "Should bad frame detection be enabled? This option will attempt to "
                    "bridge the gap between frames which don't meet certain criteria "
                    "(percentage of feature points tracked) and will instead attempt "
                    "to match features on the current frame against past frames to "
                    "meet this criteria. This is useful when there can be bad frames.");

  config->set_value("bf_detection_percent_match_req", bf_detection_percent_match_req_,
                    "The required percentage of features needed to be matched for a "
                    "stitch to be considered successful (value must be between 0.0 and "
                    "1.0).");

  config->set_value("bf_detection_new_shot_length", bf_detection_new_shot_length_,
                    "Number of frames for a new shot to be considered valid before "
                    "attempting to stitch to prior shots.");

  config->set_value("bf_detection_max_search_length", bf_detection_max_search_length_,
                    "Maximum number of frames to search in the past for matching to "
                    "the end of the last shot.");

  return config;
}


/// Set this algo's properties via a config block
void
close_loops_bad_frames_only
::set_configuration(config_block_sptr in_config)
{
  // Starting with our generated config_block to ensure that assumed values are present
  // An alternative is to check for key presence before performing a get_value() call.
  config_block_sptr config = this->get_configuration();
  config->merge_config(in_config);

  // Setting nested algorithm instances via setter methods instead of directly
  // assigning to instance property.
  match_features_sptr mf;
  match_features::set_nested_algo_configuration("feature_matcher", config, mf);
  matcher_ = mf;

  // Settings for bad frame detection
  bf_detection_enabled_ = config->get_value<bool>("bf_detection_enabled");
  bf_detection_percent_match_req_ = config->get_value<double>("bf_detection_percent_match_req");
  bf_detection_max_search_length_ = config->get_value<unsigned>("bf_detection_max_search_length");
  bf_detection_new_shot_length_ = config->get_value<unsigned>("bf_detection_new_shot_length");
  bf_detection_new_shot_length_ = ( bf_detection_new_shot_length_ ? bf_detection_new_shot_length_ : 1 );
}


bool
close_loops_bad_frames_only
::check_configuration(config_block_sptr config) const
{
  return (
    match_features::check_nested_algo_configuration("feature_matcher", config)
    &&
    std::abs( config->get_value<double>("bf_detection_percent_match_req") ) <= 1.0
  );
}


/// Functor to help remove tracks from vector
bool track_id_in_set( track_sptr trk_ptr, std::set<track_id_t>* set_ptr )
{
  return set_ptr->find( trk_ptr->id() ) != set_ptr->end();
}


/// Handle track bad frame detection if enabled
track_set_sptr
close_loops_bad_frames_only
::stitch( frame_id_t frame_number, track_set_sptr input ) const
{
  // check if enabled and possible
  if( !bf_detection_enabled_ || frame_number <= bf_detection_new_shot_length_ )
  {
    return input;
  }

  // check if we should attempt to stitch together past frames
  std::vector< track_sptr > all_tracks = input->tracks();
  frame_id_t frame_to_stitch = frame_number - bf_detection_new_shot_length_ + 1;
  double pt = input->percentage_tracked( frame_to_stitch - 1, frame_to_stitch );
  bool stitch_required = ( pt < bf_detection_percent_match_req_ );

  // confirm that the new valid shot criteria length is satisfied
  frame_id_t frame_to_test = frame_to_stitch + 1;
  while( stitch_required && frame_to_test <= frame_number )
  {
    pt = input->percentage_tracked( frame_to_test - 1, frame_to_test );
    stitch_required = ( pt >= bf_detection_percent_match_req_ );
    frame_to_test++;
  }

  // determine if a stitch can be attempted
  if( !stitch_required )
  {
    return input;
  }

  // attempt to stitch start of shot frame against past n frames
  frame_to_test = frame_to_stitch - 2;
  frame_id_t last_frame_to_test = 0;

  if( frame_to_test > bf_detection_max_search_length_ )
  {
    last_frame_to_test = frame_to_test - bf_detection_max_search_length_;
  }

  track_set_sptr stitch_frame_set = input->active_tracks( frame_to_stitch );

  for( ; frame_to_test > last_frame_to_test; frame_to_test-- )
  {
    track_set_sptr test_frame_set = input->active_tracks( frame_to_test );

    // run matcher alg
    match_set_sptr mset = matcher_->match(test_frame_set->frame_features( frame_to_test ),
                                          test_frame_set->frame_descriptors( frame_to_test ),
                                          stitch_frame_set->frame_features( frame_to_stitch ),
                                          stitch_frame_set->frame_descriptors( frame_to_stitch ));

    // test matcher results
    unsigned total_features = test_frame_set->size() + stitch_frame_set->size();

    if( 2*mset->size() >= static_cast<unsigned>(bf_detection_percent_match_req_*total_features) )
    {
      // modify track history and exit
      std::vector<track_sptr> test_frame_trks = test_frame_set->tracks();
      std::vector<track_sptr> stitch_frame_trks = stitch_frame_set->tracks();
      std::vector<match> matches = mset->matches();
      std::set<track_id_t> to_remove;

      for( unsigned i = 0; i < matches.size(); i++ )
      {
        if( test_frame_trks[ matches[i].first ]->append( *stitch_frame_trks[ matches[i].second ] ) )
        {
          to_remove.insert( stitch_frame_trks[ matches[i].second ]->id() );
        }
      }

      if( !to_remove.empty() )
      {
        std::remove_if( all_tracks.begin(), all_tracks.end(),
                        boost::bind( track_id_in_set, _1, &to_remove ) );
      }

      return track_set_sptr( new simple_track_set( all_tracks ) );
    }
  }

  // bad frame detection has failed
  return input;
}


} // end namespace algo

} // end namespace maptk
