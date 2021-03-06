#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <infiniband/umad.h>
#include <infiniband/mad.h>
#include "trace.h"

int main(int argc, char *argv[])
{
  /* /sys/class/infiniband/HCA_NAME/ports/HCA_PORT */

  char *hca_name = "mlx4_0";
  int hca_port = 1;
  int sw_lid = 3229;
  int sw_port = 9;

  int umad_fd = -1;
  int umad_agent_id = -1;
  int umad_timeout_ms = 15;
  int umad_retries = 10;

#ifdef DEBUG
  umad_debug(9);
#endif

  if (umad_init() < 0) {
    ERROR("cannot init libibumad: %m\n");
    goto out;
  }

  umad_fd = umad_open_port(hca_name, hca_port);
  if (umad_fd < 0) {
    ERROR("cannot open umad port: %m\n");
    goto out;
  }

  /* mad_register_client_via(mgmt, rmpp_version, p); */
  /* rmpp_version = 0 */
  /* mad_register_port_client(mad_fd, mgmt, rmpp_version); */
  /* vers = mgmt_class_vers(mgmt)  = 1*/

  umad_agent_id = umad_register(umad_fd, IB_PERFORMANCE_CLASS, 1, 0, 0);
  if (umad_agent_id < 0) {
    ERROR("cannot register umad agent: %m\n");
    goto out;
  }

  uint64_t s_xid = 0x1010cafebabe8008;

  char s_buf[1024], r_buf[1024];
  memset(s_buf, 0, sizeof(s_buf));
  memset(r_buf, 0, sizeof(r_buf));

  struct ib_user_mad *su = (struct ib_user_mad *) s_buf;
  umad_set_addr(su, sw_lid, 1, 0, IB_DEFAULT_QP1_QKEY);
  su->agent_id = umad_agent_id;
  su->timeout_ms = umad_timeout_ms;
  su->retries = umad_retries;

  void *sm = umad_get_mad(su);
  mad_set_field(sm, 0, IB_MAD_METHOD_F, IB_MAD_METHOD_GET);
  /* mad_set_field(sm, 0, IB_MAD_RESPONSE_F, 0); */
  /* mad_set_field(sm, 0, IB_MAD_STATUS_F, 0 ); *//* rpc->rstatus */
  mad_set_field(sm, 0, IB_MAD_CLASSVER_F, 1);
  mad_set_field(sm, 0, IB_MAD_MGMTCLASS_F, IB_PERFORMANCE_CLASS);
  mad_set_field(sm, 0, IB_MAD_BASEVER_F, 1);
  mad_set_field64(sm, 0, IB_MAD_TRID_F, s_xid);
  mad_set_field(sm, 0, IB_MAD_ATTRID_F, IB_GSI_PORT_COUNTERS_EXT);
  /* mad_set_field(sm, 0, IB_MAD_ATTRMOD_F, 0); *//* rpc->attr.mod */
  /* mad_set_field64(sm, 0, IB_MAD_MKEY_F, 0); *//* rpc->mkey */

  void *s_data = (char *) sm + IB_PC_DATA_OFFS;
  mad_set_field(s_data, 0, IB_PC_PORT_SELECT_F, sw_port);

  ssize_t nw = write(umad_fd, su, umad_size() + IB_MAD_SIZE);
  if (nw < 0) { /* ... */
    ERROR("cannot send mad: %m\n");
    goto out;
  }

  struct pollfd fds = {
    .fd = umad_fd,
    .events = POLLIN,
  };

  /* TODO Fixup timeout. */

  while (1) {
    int np = poll(&fds, 1, umad_timeout_ms);
    if (np < 0) {
      ERROR("error polling for mads: %m\n");
      goto out;
    } else if (np == 0) {
      ERROR("timedout waiting for mad\n");
      goto out;
    }

    ssize_t nr = read(umad_fd, r_buf, sizeof(r_buf));
    if (nr < 0) {
      if (errno == EWOULDBLOCK)
        continue;
      ERROR("error receiving mad: %m\n");
      goto out;
    }

    break;
  }

  struct ib_user_mad *ru = (struct ib_user_mad *) r_buf;
  void *rm = umad_get_mad(ru);
  uint64_t r_xid = mad_get_field64(rm, 0, IB_MAD_TRID_F);

  TRACE("ru status %d\n", ru->status);
  TRACE("s_xid %lx, r_xid %lx\n", (unsigned long) s_xid, (unsigned long) r_xid);

  if (mad_get_field(rm, 0, IB_DRSMP_STATUS_F) == IB_MAD_STS_REDIRECT) {
    ERROR("received redirect\n");
    goto out;
  }

  void *r_data = (char *) rm + IB_PC_DATA_OFFS;
  uint64_t sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets;
  mad_decode_field(r_data, IB_PC_EXT_RCV_BYTES_F, &sw_rx_bytes);
  mad_decode_field(r_data, IB_PC_EXT_RCV_PKTS_F,  &sw_rx_packets);
  mad_decode_field(r_data, IB_PC_EXT_XMT_BYTES_F, &sw_tx_bytes);
  mad_decode_field(r_data, IB_PC_EXT_XMT_PKTS_F,  &sw_tx_packets);

  TRACE("sw_rx_bytes %lu, sw_rx_packets %lu, sw_tx_bytes %lu, sw_tx_packets %lu\n",
        sw_rx_bytes, sw_rx_packets, sw_tx_bytes, sw_tx_packets);

 out:
  if (umad_fd >= 0)
    umad_close_port(umad_fd);

  return 0;
}
