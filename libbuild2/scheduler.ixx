// file      : libbuild2/scheduler.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

namespace build2
{
  inline scheduler::queue_mark::
  queue_mark (scheduler& s)
      : tq_ (s.queue ())
  {
    if (tq_ != nullptr)
    {
      lock ql (tq_->mutex);

      if (tq_->mark != s.task_queue_depth_)
      {
        om_ = tq_->mark;
        tq_->mark = s.task_queue_depth_;
      }
      else
        tq_ = nullptr;
    }
  }

  inline scheduler::queue_mark::
  ~queue_mark ()
  {
    if (tq_ != nullptr)
    {
      lock ql (tq_->mutex);
      tq_->mark = tq_->size == 0 ? tq_->tail : om_;
    }
  }
}
