/*
 *  Copyright (C) 2013 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>

#include "showtime.h"
#include "navigator.h"
#include "backend/backend.h"
#include "misc/str.h"
#include "misc/sha.h"
#include "misc/bytestream.h"
#include "networking/http.h"
#include "htsmsg/htsmsg.h"
#include "bittorrent.h"

#define TORRENT_REQ_SIZE 16384

bt_global_t btg;
HTS_MUTEX_DECL(bittorrent_mutex);
struct torrent_list torrents;
static int torrent_io_signal;

static int torrent_parse_metainfo(torrent_t *to, htsmsg_t *metainfo,
                                  char *errbuf, size_t errlen);

static void update_interest(torrent_t *to);
static void torrent_io_reschedule(void *aux);

static hts_cond_t torrent_piece_completed_cond;
static hts_cond_t torrent_piece_hashed_cond;

static int torrent_hash_thread_running;
//static int torrent_write_thread_running;
//static hts_thread_t torrent_read_thread;

static void bt_wakeup_hash_thread(void);
//static void bt_wakeup_write_thread(void);



/**
 *
 */
static void
torrent_add_tracker(torrent_t *to, const char *url)
{
  torrent_tracker_t *tt;

  LIST_FOREACH(tt, &to->to_trackers, tt_torrent_link) {
    if(!strcmp(tt->tt_tracker->t_url, url))
      break;
  }

  if(tt == NULL) {
    tracker_t *tr = tracker_create(url);
    if(tr != NULL)
      tracker_add_torrent(tr, to);
  }
}


/**
 *
 */
torrent_t *
torrent_create(const uint8_t *info_hash, const char *title,
	       const char **trackers, htsmsg_t *metainfo)
{
  torrent_t *to;

  hts_mutex_assert(&bittorrent_mutex);

  LIST_FOREACH(to, &torrents, to_link) {
    if(!memcmp(to->to_info_hash, info_hash, 20))
      break;
  }

  if(to == NULL) {
    to = calloc(1, sizeof(torrent_t));
    memcpy(to->to_info_hash, info_hash, 20);
    LIST_INSERT_HEAD(&torrents, to, to_link);
    TAILQ_INIT(&to->to_inactive_peers);
    TAILQ_INIT(&to->to_disconnected_peers);
    TAILQ_INIT(&to->to_connect_failed_peers);
    TAILQ_INIT(&to->to_files);
    TAILQ_INIT(&to->to_root);
    TAILQ_INIT(&to->to_active_pieces);

    to->to_title = malloc(41);
    bin2hex(to->to_title, 40, info_hash, 20);
    to->to_title[40] = 0;

    asyncio_timer_init(&to->to_io_reschedule, torrent_io_reschedule, to);

  }

  to->to_refcount++;

  if(metainfo != NULL) {
    if(torrent_parse_metainfo(to, metainfo,
                              to->to_errbuf, sizeof(to->to_errbuf))) {
      TRACE(TRACE_ERROR, "BT", "Unable to parse torrent: %s",
            to->to_errbuf);
    }
  }

  if(trackers != NULL) {
    for(;*trackers; trackers++) {
      torrent_add_tracker(to, *trackers);
    }
  }

  return to;
}


/**
 *
 */
void
torrent_release(torrent_t *t)
{
  printf("Release not implemnted\n");
}



/**
 *
 */
static void
block_cancel_requests(torrent_block_t *tb)
{
  torrent_request_t *tr;
  while((tr = LIST_FIRST(&tb->tb_requests)) != NULL) {
    assert(tr->tr_block == tb);
    LIST_REMOVE(tr, tr_block_link);
    tr->tr_block = NULL;

    // If we don't know the response delay yet, then keep the request
    // around just for measurement purposes
    if(tr->tr_peer->p_block_delay == 0)
      continue;

    peer_cancel_request(tr);
  }
}


/**
 *
 */
static void
block_destroy(torrent_block_t *tb)
{
  LIST_REMOVE(tb, tb_piece_link);
  assert(LIST_FIRST(&tb->tb_requests) == NULL);
  free(tb);
}



/**
 *
 */
void
torrent_receive_block(torrent_block_t *tb, const void *buf,
                      int begin, int len, torrent_t *to)
{
  int second = async_now / 1000000;

  torrent_piece_t *tp = tb->tb_piece;

  tp->tp_downloaded_bytes += len;
  average_fill(&tp->tp_download_rate, second, tp->tp_downloaded_bytes);

  memcpy(tp->tp_data + begin, buf, len);

  // If there are any other requests for this block, cancel them
  block_cancel_requests(tb);
  block_destroy(tb);

  if(LIST_FIRST(&tp->tp_waiting_blocks) == NULL &&
     LIST_FIRST(&tp->tp_sent_blocks) == NULL) {

    // Piece complete

    tp->tp_complete = 1;
    hts_cond_broadcast(&torrent_piece_completed_cond);

    bt_wakeup_hash_thread();
  }
  torrent_io_do_requests(to);
}



/**
 *
 */
void
torrent_attempt_more_peers(torrent_t *to)
{
  if(to->to_active_peers  >= btg.btg_max_peers_torrent ||
     btg.btg_active_peers >= btg.btg_max_peers_global)
    return;

  peer_t *p;

  p = TAILQ_FIRST(&to->to_inactive_peers);
  if(p != NULL) {
    TAILQ_REMOVE(&to->to_inactive_peers, p, p_queue_link);
    peer_connect(p);
    return;
  }

  p = TAILQ_FIRST(&to->to_disconnected_peers);
  if(p != NULL) {
    TAILQ_REMOVE(&to->to_disconnected_peers, p, p_queue_link);
    peer_connect(p);
    return;
  }

  p = TAILQ_FIRST(&to->to_connect_failed_peers);
  if(p != NULL) {
    TAILQ_REMOVE(&to->to_connect_failed_peers, p, p_queue_link);
    peer_connect(p);
    return;
  }
}


/**
 *
 */
static int
torrent_parse_metainfo(torrent_t *to, htsmsg_t *metainfo,
                       char *errbuf, size_t errlen)
{
  htsmsg_t *info = htsmsg_get_map(metainfo, "info");
  if(info == NULL) {
    snprintf(errbuf, errlen, "Missing info dict");
    return 1;
  }

  const char *name = htsmsg_get_str(info, "name");
  if(name != NULL)
    mystrset(&to->to_title, name);


  htsmsg_t *al = htsmsg_get_list(metainfo, "announce-list");
  if(al != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, al) {
      htsmsg_t *l = htsmsg_get_list_by_field(f);
      if(l == NULL)
        continue;
      htsmsg_field_t *ff;
      HTSMSG_FOREACH(ff, l) {
        const char *t = htsmsg_field_get_string(ff);
        if(t != NULL) {
          torrent_add_tracker(to, t);
        }
      }
    }
  } else {
    const char *announce = htsmsg_get_str(metainfo, "announce");
    if(announce != NULL)
      torrent_add_tracker(to, announce);
  }

  htsmsg_t *files = htsmsg_get_list(info, "files");

  uint64_t offset = 0;

  if(files != NULL) {
    // Multi file torrent

    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, files) {
      htsmsg_t *file = htsmsg_get_map_by_field(f);
      if(file == NULL) {
        snprintf(errbuf, errlen, "File is not a dict");
        return 1;
      }
      int64_t length;
      if(htsmsg_get_s64(file, "length", &length)) {
        snprintf(errbuf, errlen, "Missing file length");
        return 1;
      }

      if(length < 0) {
        snprintf(errbuf, errlen, "Invalid file length");
        return 1;
      }

      htsmsg_t *paths = htsmsg_get_list(file, "path");

      torrent_file_t *tf = NULL;

      htsmsg_field_t *ff;
      char *filename = NULL;

      HTSMSG_FOREACH(ff, paths) {
        const char *path = htsmsg_field_get_string(ff);
        if(path == NULL) {
          snprintf(errbuf, errlen, "Path component is not a string");
          return 1;
        }

        if(filename != NULL)
          strappend(&filename, "/");
        strappend(&filename, path);

        struct torrent_file_queue *tfq = tf ? &tf->tf_files : &to->to_root;
        TAILQ_FOREACH(tf, tfq, tf_parent_link) {
          if(!strcmp(tf->tf_name, path))
            break;
        }
        if(tf == NULL) {
          tf = calloc(1, sizeof(torrent_file_t));
          tf->tf_torrent = to;
          TAILQ_INIT(&tf->tf_files);
          tf->tf_name = strdup(path);
          TAILQ_INSERT_TAIL(&to->to_files, tf, tf_torrent_link);
          TAILQ_INSERT_TAIL(tfq, tf, tf_parent_link);
          tf->tf_fullpath = strdup(filename);
        }
      }

      tf->tf_offset = offset;
      tf->tf_size = length;
      offset += length;
      free(filename);
    }
  } else {
    const char *name = htsmsg_get_str(info, "name");

    if(name == NULL) {
      snprintf(errbuf, errlen, "Missing file name");
      return 1;
    }

    int64_t length;

    if(htsmsg_get_s64(info, "length", &length)) {
      snprintf(errbuf, errlen, "Missing file length");
      return 1;
    }

    if(length < 0) {
      snprintf(errbuf, errlen, "Invalid file length");
      return 1;
    }

    torrent_file_t *tf = calloc(1, sizeof(torrent_file_t));
    tf->tf_torrent = to;
    TAILQ_INIT(&tf->tf_files);
    tf->tf_name = strdup(name);
    TAILQ_INSERT_TAIL(&to->to_files, tf, tf_torrent_link);
    TAILQ_INSERT_TAIL(&to->to_root, tf, tf_parent_link);

    char *filename = NULL;
    strappend(&filename, "/");
    strappend(&filename, name);

    tf->tf_offset = 0;
    tf->tf_size = length;
    offset = length;
    tf->tf_fullpath = filename;
  }

  to->to_total_length = offset;

  to->to_piece_length = htsmsg_get_u32_or_default(info, "piece length", 0);
  if(to->to_piece_length < 32768 || to->to_piece_length > 8388608) {
    snprintf(errbuf, errlen, "Invalid piece length: %d", to->to_piece_length);
    return 1;
  }

  const void *pieces_data;
  size_t pieces_size;
  if(htsmsg_get_bin(info, "pieces", &pieces_data, &pieces_size)) {
    snprintf(errbuf, errlen, "No hash list");
    return 1;
  }

  if(pieces_size % 20) {
    snprintf(errbuf, errlen, "Invalid hash list size: %zd", pieces_size);
    return 1;
  }

  to->to_num_pieces = pieces_size / 20;
  to->to_piece_flags = calloc(1, to->to_num_pieces);

  to->to_piece_hashes = malloc(pieces_size);
  memcpy(to->to_piece_hashes, pieces_data, pieces_size);

  return 0;
}



/**
 *
 */
static int
tp_deadline_cmp(const torrent_piece_t *a, const torrent_piece_t *b)
{
  if(a->tp_deadline < b->tp_deadline)
    return -1;
  return a->tp_deadline >= b->tp_deadline;
}

/**
 *
 */
static torrent_piece_t *
torrent_piece_find(torrent_t *to, int piece_index)
{
  torrent_piece_t *tp;
  TAILQ_FOREACH(tp, &to->to_active_pieces, tp_link) {
    if(tp->tp_index == piece_index) {
      TAILQ_REMOVE(&to->to_active_pieces, tp, tp_link);
      TAILQ_INSERT_TAIL(&to->to_active_pieces, tp, tp_link);
      return tp;
    }
  }

  to->to_num_active_pieces++;
  tp = calloc(1, sizeof(torrent_piece_t));
  tp->tp_index = piece_index;
  TAILQ_INSERT_TAIL(&to->to_active_pieces, tp, tp_link);
  tp->tp_deadline = INT64_MAX;
  LIST_INSERT_SORTED(&to->to_serve_order, tp, tp_serve_link, tp_deadline_cmp);

  tp->tp_data = malloc(to->to_piece_length);

  // Create and enqueue requests

  tp->tp_piece_length = to->to_piece_length;

  if(piece_index == to->to_num_pieces - 1) {
    // Last piece, truncate piece length
    tp->tp_piece_length = to->to_total_length % to->to_piece_length;
  }

  for(int i = 0; i < tp->tp_piece_length; i += TORRENT_REQ_SIZE) {
    torrent_block_t *tb = calloc(1, sizeof(torrent_block_t));
    tb->tb_piece = tp;
    LIST_INSERT_HEAD(&tp->tp_waiting_blocks, tb, tb_piece_link);
    tb->tb_begin = i;
    tb->tb_length = MIN(TORRENT_REQ_SIZE, tp->tp_piece_length - i);
  }

  update_interest(to);

  return tp;
}

/**
 *
 */
static void
piece_update_deadline(torrent_t *to, torrent_piece_t *tp)
{
  int64_t deadline = INT64_MAX;
  torrent_fh_t *tfh;
  LIST_FOREACH(tfh, &tp->tp_active_fh, tfh_piece_link)
    deadline = MIN(tfh->tfh_deadline, deadline);

  if(tp->tp_deadline == deadline)
    return;

  tp->tp_deadline = deadline;

  LIST_REMOVE(tp, tp_serve_link);
  LIST_INSERT_SORTED(&to->to_serve_order, tp, tp_serve_link, tp_deadline_cmp);
}



/**
 *
 */
int
torrent_load(torrent_t *to, void *buf, uint64_t offset, size_t size,
	     torrent_fh_t *tfh)
{
  int rval = size;
  // First figure out which pieces we need

  int piece = offset        / to->to_piece_length;
  int piece_offset = offset % to->to_piece_length;

  // Poor mans read-ahead
  if(piece + 1 < to->to_num_pieces)
    torrent_piece_find(to, piece + 1);
  if(piece + 2 < to->to_num_pieces)
    torrent_piece_find(to, piece + 2);
  while(size > 0) {

    torrent_piece_t *tp = torrent_piece_find(to, piece);

    tp->tp_refcount++;

    LIST_INSERT_HEAD(&tp->tp_active_fh, tfh, tfh_piece_link);

    piece_update_deadline(to, tp);

    if(!tp->tp_hash_computed) {
      asyncio_wakeup_worker(torrent_io_signal);

      while(!tp->tp_hash_computed)
        hts_cond_wait(&torrent_piece_hashed_cond, &bittorrent_mutex);
    }

    LIST_REMOVE(tfh, tfh_piece_link);

    piece_update_deadline(to, tp);

    tp->tp_refcount--;

    int copy = MIN(size, to->to_piece_length - piece_offset);

    memcpy(buf, tp->tp_data + piece_offset, copy);

    piece++;
    piece_offset = 0;
    size -= copy;
    buf += copy;
  }

  return rval;
}



/**
 *
 */
static void
update_interest(torrent_t *to)
{
  peer_t *p;

  LIST_FOREACH(p, &to->to_running_peers, p_running_link)
    peer_update_interest(to, p);
}


/**
 *
 */
static void
add_request(torrent_block_t *tb, peer_t *p)
{
  torrent_request_t *tr;
  LIST_FOREACH(tr, &p->p_requests, tr_peer_link) {
    if(tr->tr_block == tb)
      return; // Request already sent
  }

  tr = calloc(1, sizeof(torrent_request_t));

  tr->tr_piece  = tb->tb_piece->tp_index;
  tr->tr_begin  = tb->tb_begin;
  tr->tr_length = tb->tb_length;

  tr->tr_block = tb;

  tr->tr_send_time = async_now;
  tr->tr_req_num = tb->tb_req_tally;
  tb->tb_req_tally++;

  LIST_INSERT_HEAD(&tb->tb_requests, tr, tr_block_link);
  peer_send_request(p, tb);

  tr->tr_peer = p;
  tr->tr_qdepth = p->p_active_requests;

  LIST_INSERT_HEAD(&p->p_requests, tr, tr_peer_link);
  p->p_active_requests++;

  p->p_last_send = async_now;
}


/**
 *
 */
static peer_t *
find_optimal_peer(torrent_t *to, const torrent_piece_t *tp)
{
  int best_score = INT32_MAX;
  peer_t *best = NULL;
  peer_t *p;

  LIST_FOREACH(p, &to->to_unchoked_peers, p_unchoked_link) {

    if(p->p_piece_flags == NULL ||
       !(p->p_piece_flags[tp->tp_index] & PIECE_HAVE))
      continue;

    int score;

    if(p->p_block_delay == 0) {
      // Delay not known yet
      
      if(p->p_active_requests) {
	// We have a request already, skip this peer
	continue;
      }

      score = 0; // Assume it's super fast
    } else {

      score = p->p_block_delay;
    }

    if(best == NULL || score < best_score) {
      best = p;
      best_score = score;
    }
  }
  return best;
}


/**
 *
 */
static peer_t *
find_any_peer(torrent_t *to, const torrent_piece_t *tp)
{
  peer_t *p;

  LIST_FOREACH(p, &to->to_unchoked_peers, p_unchoked_link) {

    if(p->p_piece_flags == NULL ||
       !(p->p_piece_flags[tp->tp_index] & PIECE_HAVE))
      continue;

    if(p->p_active_requests < p->p_maxq / 2)
      return p;
  }
  return NULL;
}


#if 1
static const char *
block_name(const torrent_block_t *tb)
{
  static char buf[128];
  snprintf(buf, sizeof(buf), "piece:%d block:0x%x+0x%x",
           tb->tb_piece->tp_index, tb->tb_begin, tb->tb_length);
  return buf;
}
#endif


/**
 *
 */
static void
serve_waiting_blocks(torrent_t *to, torrent_piece_t *tp, int optimal)
{
  torrent_block_t *tb, *next;

  // First take a round and figure out the best peer for where
  // to schedule a read

  for(tb = LIST_FIRST(&tp->tp_waiting_blocks); tb != NULL; tb = next) {
    next = LIST_NEXT(tb, tb_piece_link);

    if(optimal) {
      peer_t *p = find_optimal_peer(to, tp);

      if(p == NULL || p->p_active_requests >= p->p_maxq)
	break;

      add_request(tb, p);

    } else {
      peer_t *p = find_any_peer(to, tp);
      if(p == NULL)
	break;

      add_request(tb, p);

    }
    LIST_REMOVE(tb, tb_piece_link);
    LIST_INSERT_HEAD(&tp->tp_sent_blocks, tb, tb_piece_link);
  }
}




/**
 *
 */
static peer_t *
find_faster_peer(torrent_t *to, const torrent_block_t *tb,
		 int64_t eta_to_beat)
{
  peer_t *p, *best = NULL;
  const torrent_piece_t *tp = tb->tb_piece;
  assert(tp != NULL);

  LIST_FOREACH(p, &to->to_unchoked_peers, p_unchoked_link) {

    if(p->p_piece_flags == NULL ||
       !(p->p_piece_flags[tp->tp_index] & PIECE_HAVE))
      continue;

    if(p->p_block_delay == 0) {
      // Delay not known yet
      continue;
    }

    if(p->p_active_requests >= p->p_maxq)
      continue; // Peer is fully queued


    const torrent_request_t *tr;
    LIST_FOREACH(tr, &tb->tb_requests, tr_block_link) {
      if(tr->tr_peer == p)
	break;
    }
    if(tr != NULL)
      continue;

    int64_t t = async_now + p->p_block_delay * 2;

    if(t < eta_to_beat) {
      eta_to_beat = t;
      best = p;
    }
  }
  return best;
}


/**
 *
 */
static void
check_active_requests(torrent_t *to, torrent_piece_t *tp,
		      int64_t deadline)
{
  torrent_block_t *tb, *next;

  for(tb = LIST_FIRST(&tp->tp_sent_blocks); tb != NULL; tb = next) {
    next = LIST_NEXT(tb, tb_piece_link);
    /*
     * The most recent request for the block is always first in
     * this list. It's the only one we will compare with since
     * we are only gonna add duplicate requests if we think we can
     * outrun the currently enqueued requests
     */
    torrent_request_t *cur = LIST_FIRST(&tb->tb_requests);
    assert(cur != NULL);

    peer_t *curpeer = cur->tr_peer;
    assert(curpeer != NULL);

    int64_t eta, delay = 0;

    eta = cur->tr_send_time + curpeer->p_block_delay;
    if(eta < async_now) {
      // Didn't arrive on time, assume the delay will worse
      // the longer it takes
      delay = async_now - eta;
      eta += delay * 2;
    }

    if(eta < deadline)
      continue; // Nothing to worry about

    // Now, let's see if we can find a peer that we think can beat
    // the current (offsetted) ETA for this block

    peer_t *p = find_faster_peer(to, tb, eta);
    if(p == NULL)
      continue;

    if(0)
    printf("Block %s: Added dup request on peer %s bd:%d "
	   "computed ETA:%ld delay:%ld\n",
	   block_name(tb), p->p_name, p->p_block_delay,
	   eta - async_now, delay);

    add_request(tb, p);

    int new_delay = async_now - cur->tr_send_time;
    if(new_delay > curpeer->p_block_delay) {
      curpeer->p_block_delay = (curpeer->p_block_delay * 7 + new_delay) / 8;
    }
  }
}


/**
 *
 */
static void
torrent_piece_destroy(torrent_t *to, torrent_piece_t *tp)
{
  assert(LIST_FIRST(&tp->tp_active_fh) == NULL);
  to->to_num_active_pieces--;

  free(tp->tp_data);
  TAILQ_REMOVE(&to->to_active_pieces, tp, tp_link);
  LIST_REMOVE(tp, tp_serve_link);
  free(tp);
}


/**
 *
 */
static void
flush_active_pieces(torrent_t *to)
{
  while(to->to_num_active_pieces > 20) {
    torrent_piece_t *tp = TAILQ_FIRST(&to->to_active_pieces);
    assert(tp != NULL);

    if(tp->tp_refcount)
      break;

    if(LIST_FIRST(&tp->tp_waiting_blocks) != NULL)
      break;

    if(LIST_FIRST(&tp->tp_sent_blocks) != NULL)
      break;

    torrent_piece_destroy(to, tp);
  }
}


/**
 *
 */
static void
torrent_send_have(torrent_t *to)
{
  torrent_piece_t *tp;

  TAILQ_FOREACH(tp, &to->to_active_pieces, tp_link) {
    if(!tp->tp_hash_ok)
      continue;

    peer_t *p;

    const int pid = tp->tp_index;

    LIST_FOREACH(p, &to->to_running_peers, p_running_link) {

      if(p->p_piece_flags == NULL)
        p->p_piece_flags = calloc(1, to->to_num_pieces);

      if(p->p_piece_flags[pid] & PIECE_NOTIFIED)
        continue;
      peer_send_have(p, pid);
      p->p_piece_flags[pid] |= PIECE_NOTIFIED;
    }
  }
}


/**
 *
 */
static void
torrent_unchoke_peers(torrent_t *to)
{
  peer_t *p;
  LIST_FOREACH(p, &to->to_running_peers, p_running_link) {

    int choke = 1;

    if(p->p_num_pieces_have != to->to_num_pieces &&
       p->p_peer_interested)
      choke = 0;

    peer_choke(p, choke);
  }
}


/**
 *
 */
void
torrent_io_do_requests(torrent_t *to)
{
  torrent_piece_t *tp;
#if 0
  printf("----------------------------------------\n");
  LIST_FOREACH(tp, &to->to_serve_order, tp_serve_link) {
    printf("Processing piece %d  deadline:%ld files:%s\n",
	   tp->tp_index, tp->tp_deadline,
	   LIST_FIRST(&tp->tp_active_fh) ? "YES" : "NO");
  }
  printf("----------------------------------------\n");
#endif

  LIST_FOREACH(tp, &to->to_serve_order, tp_serve_link) {
    if(tp->tp_deadline == INT64_MAX)
      break;
    check_active_requests(to, tp, tp->tp_deadline);
  }

  LIST_FOREACH(tp, &to->to_serve_order, tp_serve_link)
    serve_waiting_blocks(to, tp, 1);

  LIST_FOREACH(tp, &to->to_serve_order, tp_serve_link)
    serve_waiting_blocks(to, tp, 0);

  int second = async_now / 1000000;

  int rate = average_read(&to->to_download_rate, second) / 125;

  torrent_fh_t *tfh;
  LIST_FOREACH(tfh, &to->to_fhs, tfh_torrent_link) {
    if(tfh->tfh_fa_stats != NULL) {
      prop_set(tfh->tfh_fa_stats, "bitrate", PROP_SET_INT, rate);
    }
  }

  flush_active_pieces(to);

  asyncio_timer_arm(&to->to_io_reschedule, async_now + 1000000);

  if(to->to_last_unchoke_check + 5 < second) {
    to->to_last_unchoke_check = second;
    torrent_unchoke_peers(to);
  }
}


/**
 *
 */
static void
torrent_io_check_pendings(void)
{
  hts_mutex_lock(&bittorrent_mutex);

  torrent_t *to;
  LIST_FOREACH(to, &torrents, to_link) {

    if(to->to_new_valid_piece) {
      to->to_new_valid_piece = 0;
      torrent_send_have(to);
    }

    torrent_io_do_requests(to);
  }

  hts_mutex_unlock(&bittorrent_mutex);
}


/**
 *
 */
static void
torrent_io_reschedule(void *aux)
{
  hts_mutex_lock(&bittorrent_mutex);
  torrent_io_do_requests(aux);
  hts_mutex_unlock(&bittorrent_mutex);
}


/**
 *
 */
static void
torrent_piece_verify_hash(torrent_t *to, torrent_piece_t *tp)
{
  uint8_t digest[20];
  sha1_decl(shactx);

  tp->tp_refcount++;
  hts_mutex_unlock(&bittorrent_mutex);
  int64_t ts = showtime_get_ts();

  sha1_init(shactx);
  sha1_update(shactx, tp->tp_data, tp->tp_piece_length);
  sha1_final(shactx, digest);

  ts = showtime_get_ts() - ts;

  hts_mutex_lock(&bittorrent_mutex);

  tp->tp_hash_computed = 1;

  const uint8_t *piecehash = to->to_piece_hashes + tp->tp_index * 20;
  tp->tp_hash_ok = !memcmp(piecehash, digest, 20);
  tp->tp_refcount--;
  if(!tp->tp_hash_ok) {
    TRACE(TRACE_ERROR, "BITTORRENT", "Received corrupt piece %d",
	  tp->tp_index);
  } else {
    to->to_new_valid_piece = 1;
    asyncio_wakeup_worker(torrent_io_signal);
  }

  hts_cond_broadcast(&torrent_piece_hashed_cond);

  // bt_wakeup_write_thread();
}


/**
 *
 */
static void *
bt_hash_thread(void *aux)
{
  torrent_t *to;

  hts_mutex_lock(&bittorrent_mutex);

  while(1) {

  restart:

    LIST_FOREACH(to, &torrents, to_link) {
      torrent_piece_t *tp;
      TAILQ_FOREACH(tp, &to->to_active_pieces, tp_link) {
	if(tp->tp_complete && !tp->tp_hash_computed) {
	  torrent_piece_verify_hash(to, tp);
	  goto restart;
	}
      }
    }

    if(hts_cond_wait_timeout(&torrent_piece_completed_cond,
			     &bittorrent_mutex, 60000))
      break;
  }

  torrent_hash_thread_running = 0;
  hts_mutex_unlock(&bittorrent_mutex);
  return NULL;
}


/**
 *
 */
static void
bt_wakeup_hash_thread(void)
{
  if(!torrent_hash_thread_running) {
    torrent_hash_thread_running = 1;
    hts_thread_create_detached("bthasher", bt_hash_thread, NULL,
			       THREAD_PRIO_BGTASK);
  }
}

#if 0

/**
 *
 */
static void
torrent_write_to_disk(torrent_t *to, torrent_piece_t *tp)
{
  torrent_file_t *tf;

  uint64_t piece_offset = tp->tp_index * to->to_piece_length;
  

  TAILQ_FOREACH(tf, &to->to_files, tf_torrent_link) {
    

  }

}

/**
 *
 */
static void *
bt_write_thread(void *aux)
{
  torrent_t *to;

  hts_mutex_lock(&bittorrent_mutex);

  while(1) {

  restart:

    LIST_FOREACH(to, &torrents, to_link) {
      torrent_piece_t *tp;
      TAILQ_FOREACH(tp, &to->to_active_pieces, tp_link) {
	if(tp->tp_hash_ok && !tp->tp_on_disk && !tp->tp_disk_fail) {
	  torrent_write_to_disk(to, tp);
	  goto restart;
	}
      }
    }

    if(hts_cond_wait_timeout(&torrent_piece_hashed_cond,
			     &bittorrent_mutex, 60000))
      break;
  }

  torrent_hash_thread_running = 0;
  hts_mutex_unlock(&bittorrent_mutex);
  return NULL;
}


/**
 *
 */
static void
bt_wakeup_write_thread(void)
{
  if(!torrent_write_thread_running) {
    torrent_write_thread_running = 1;
    hts_thread_create_detached("btwriteer", bt_write_thread, NULL,
			       THREAD_PRIO_BGTASK);
  }
}

#endif



/**
 *
 */
static void
torrent_io_init(void)
{
  btg.btg_max_peers_global = 200;
  btg.btg_max_peers_torrent = 50;

  torrent_io_signal = asyncio_add_worker(torrent_io_check_pendings);
  hts_cond_init(&torrent_piece_completed_cond, &bittorrent_mutex);
  hts_cond_init(&torrent_piece_hashed_cond, &bittorrent_mutex);

}

INITME(INIT_GROUP_ASYNCIO, torrent_io_init);
