// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#include <memory>

#include "osd/mClockOpClassQueue.h"
#include "common/dout.h"
#include "common/ignore_warning.h"


namespace dmc = crimson::dmclock;


#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix *_dout


namespace ceph {


  // this is only used for debugging and not outside this class
  START_UNUSED_FUNC
  static std::ostream& operator<<(std::ostream& out, const Request& r) {
    out << "{ pg:" << r.first->info.pgid << ", pg_queueable:{" <<
      r.second << "} }";
    return out;
  }
  END_UNUSED_FUNC

  mClockOpClassQueue::mclock_op_tags_t::mclock_op_tags_t(CephContext *cct) :
    client_op(cct->_conf->osd_op_queue_mclock_client_op_res,
	      cct->_conf->osd_op_queue_mclock_client_op_wgt,
	      cct->_conf->osd_op_queue_mclock_client_op_lim),
    osd_subop(cct->_conf->osd_op_queue_mclock_osd_subop_res,
	      cct->_conf->osd_op_queue_mclock_osd_subop_wgt,
	      cct->_conf->osd_op_queue_mclock_osd_subop_lim),
    snaptrim(cct->_conf->osd_op_queue_mclock_snap_res,
	     cct->_conf->osd_op_queue_mclock_snap_wgt,
	     cct->_conf->osd_op_queue_mclock_snap_lim),
    recov(cct->_conf->osd_op_queue_mclock_recov_res,
	  cct->_conf->osd_op_queue_mclock_recov_wgt,
	  cct->_conf->osd_op_queue_mclock_recov_lim),
    scrub(cct->_conf->osd_op_queue_mclock_scrub_res,
	  cct->_conf->osd_op_queue_mclock_scrub_wgt,
	  cct->_conf->osd_op_queue_mclock_scrub_lim)
  {
    dout(20) <<
      "mClockOpClassQueue settings:: " <<
      "client_op:" << client_op <<
      "; osd_subop:" << osd_subop <<
      "; snaptrim:" << snaptrim <<
      "; recov:" << recov <<
      "; scrub:" << scrub <<
      dendl;
  }


  dmc::ClientInfo
  mClockOpClassQueue::op_class_client_info_f(const osd_op_type_t& op_type) {
    switch(op_type) {
    case osd_op_type_t::client_op:
      return mclock_op_tags->client_op;
    case osd_op_type_t::osd_subop:
      return mclock_op_tags->osd_subop;
    case osd_op_type_t::bg_snaptrim:
      return mclock_op_tags->snaptrim;
    case osd_op_type_t::bg_recovery:
      return mclock_op_tags->recov;
    case osd_op_type_t::bg_scrub:
      return mclock_op_tags->scrub;
    default:
      assert(0);
      return dmc::ClientInfo(-1, -1, -1);
    }
  }

  /*
   * class mClockOpClassQueue
   */

  std::unique_ptr<mClockOpClassQueue::mclock_op_tags_t>
  mClockOpClassQueue::mclock_op_tags(nullptr);

  mClockOpClassQueue::pg_queueable_visitor_t
  mClockOpClassQueue::pg_queueable_visitor;

  mClockOpClassQueue::mClockOpClassQueue(CephContext *cct) :
    queue(&mClockOpClassQueue::op_class_client_info_f),
    cost_factor(cct->_conf->osd_op_queue_mclock_cost_factor)
  {
    // manage the singleton
    if (!mclock_op_tags) {
      mclock_op_tags.reset(new mclock_op_tags_t(cct));
    }
  }

  mClockOpClassQueue::osd_op_type_t
  mClockOpClassQueue::get_osd_op_type(const Request& request) {
    osd_op_type_t type =
      boost::apply_visitor(pg_queueable_visitor, request.second.get_variant());

    // if we got client_op back then we need to distinguish between
    // a client op and an osd subop.

    if (osd_op_type_t::client_op != type) {
      return type;
    } else {
      auto& op_type =
	boost::get<OpRequestRef>(request.second.get_variant())->get_req()->
	get_header().type;
      if (MSG_OSD_SUBOP == op_type) {
	return osd_op_type_t::osd_subop;
      } else if (MSG_OSD_PG_PUSH == op_type ||
		 MSG_OSD_PG_PULL == op_type) {
	return osd_op_type_t::bg_recovery;
      } else {
	return osd_op_type_t::client_op;
      }
    }
  }

  // Formatted output of the queue
  void mClockOpClassQueue::dump(ceph::Formatter *f) const {
    queue.dump(f);
  }

  inline void mClockOpClassQueue::enqueue_strict(Client cl,
						 unsigned priority,
						 Request item) {
    auto t = get_osd_op_type(item);
    queue.enqueue_strict(t, 0, item);
#if 0
    if (osd_op_type_t::bg_recovery == t) {
      dout(0) << "mclock enqueue_strict recover op " << item << dendl;
    }
#endif
  }

  // Enqueue op in the back of the regular queue
  inline void mClockOpClassQueue::enqueue(Client cl,
					  unsigned priority,
					  unsigned cost,
					  Request item) {
    auto t = get_osd_op_type(item);
    queue.enqueue(t, priority, cost, item);
#if 0
    if (osd_op_type_t::bg_recovery == t) {
      dout(0) << "mclock enqueue recover op " << item << dendl;
    }
#endif
  }

  // Return an op to be dispatch
  inline Request mClockOpClassQueue::dequeue() {
    Request result = queue.dequeue();
#if 0
    auto t = get_osd_op_type(result);
    if (osd_op_type_t::bg_recovery == t) {
      dout(0) << "mclock dequeue recover op " << result << dendl;
    }
#endif
    return result;
  }

} // namespace ceph
