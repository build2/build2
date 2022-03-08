// -*- C++ -*-
//
// This file was generated by CLI, a command line interface
// compiler for C++.
//

// Begin prologue.
//
//
// End prologue.

namespace build2
{
  // options
  //

  inline const uint64_t& options::
  build2_metadata () const
  {
    return this->build2_metadata_;
  }

  inline bool options::
  build2_metadata_specified () const
  {
    return this->build2_metadata_specified_;
  }

  inline const bool& options::
  v () const
  {
    return this->v_;
  }

  inline const bool& options::
  V () const
  {
    return this->V_;
  }

  inline const bool& options::
  quiet () const
  {
    return this->quiet_;
  }

  inline const bool& options::
  silent () const
  {
    return this->silent_;
  }

  inline const uint16_t& options::
  verbose () const
  {
    return this->verbose_;
  }

  inline bool options::
  verbose_specified () const
  {
    return this->verbose_specified_;
  }

  inline const bool& options::
  stat () const
  {
    return this->stat_;
  }

  inline const std::set<string>& options::
  dump () const
  {
    return this->dump_;
  }

  inline bool options::
  dump_specified () const
  {
    return this->dump_specified_;
  }

  inline const bool& options::
  progress () const
  {
    return this->progress_;
  }

  inline const bool& options::
  no_progress () const
  {
    return this->no_progress_;
  }

  inline const size_t& options::
  jobs () const
  {
    return this->jobs_;
  }

  inline bool options::
  jobs_specified () const
  {
    return this->jobs_specified_;
  }

  inline const size_t& options::
  max_jobs () const
  {
    return this->max_jobs_;
  }

  inline bool options::
  max_jobs_specified () const
  {
    return this->max_jobs_specified_;
  }

  inline const size_t& options::
  queue_depth () const
  {
    return this->queue_depth_;
  }

  inline bool options::
  queue_depth_specified () const
  {
    return this->queue_depth_specified_;
  }

  inline const string& options::
  file_cache () const
  {
    return this->file_cache_;
  }

  inline bool options::
  file_cache_specified () const
  {
    return this->file_cache_specified_;
  }

  inline const size_t& options::
  max_stack () const
  {
    return this->max_stack_;
  }

  inline bool options::
  max_stack_specified () const
  {
    return this->max_stack_specified_;
  }

  inline const bool& options::
  serial_stop () const
  {
    return this->serial_stop_;
  }

  inline const bool& options::
  dry_run () const
  {
    return this->dry_run_;
  }

  inline const bool& options::
  match_only () const
  {
    return this->match_only_;
  }

  inline const bool& options::
  no_external_modules () const
  {
    return this->no_external_modules_;
  }

  inline const structured_result_format& options::
  structured_result () const
  {
    return this->structured_result_;
  }

  inline bool options::
  structured_result_specified () const
  {
    return this->structured_result_specified_;
  }

  inline const bool& options::
  mtime_check () const
  {
    return this->mtime_check_;
  }

  inline const bool& options::
  no_mtime_check () const
  {
    return this->no_mtime_check_;
  }

  inline const bool& options::
  no_column () const
  {
    return this->no_column_;
  }

  inline const bool& options::
  no_line () const
  {
    return this->no_line_;
  }

  inline const path& options::
  buildfile () const
  {
    return this->buildfile_;
  }

  inline bool options::
  buildfile_specified () const
  {
    return this->buildfile_specified_;
  }

  inline const path& options::
  config_guess () const
  {
    return this->config_guess_;
  }

  inline bool options::
  config_guess_specified () const
  {
    return this->config_guess_specified_;
  }

  inline const path& options::
  config_sub () const
  {
    return this->config_sub_;
  }

  inline bool options::
  config_sub_specified () const
  {
    return this->config_sub_specified_;
  }

  inline const string& options::
  pager () const
  {
    return this->pager_;
  }

  inline bool options::
  pager_specified () const
  {
    return this->pager_specified_;
  }

  inline const strings& options::
  pager_option () const
  {
    return this->pager_option_;
  }

  inline bool options::
  pager_option_specified () const
  {
    return this->pager_option_specified_;
  }

  inline const string& options::
  options_file () const
  {
    return this->options_file_;
  }

  inline bool options::
  options_file_specified () const
  {
    return this->options_file_specified_;
  }

  inline const dir_path& options::
  default_options () const
  {
    return this->default_options_;
  }

  inline bool options::
  default_options_specified () const
  {
    return this->default_options_specified_;
  }

  inline const bool& options::
  no_default_options () const
  {
    return this->no_default_options_;
  }

  inline const bool& options::
  help () const
  {
    return this->help_;
  }

  inline const bool& options::
  version () const
  {
    return this->version_;
  }
}

// Begin epilogue.
//
//
// End epilogue.
