#include <errno.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <netlink/attr.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "nl80211.h"
#define SOL 299792458
struct nl80211_state {
    struct nl_sock *nl_sock;
    int nl80211_id;
};

static int error_handler(struct sockaddr_nl *nla, struct nlmsgerr *err,
                         void *arg) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)err - 1;
    int len = nlh->nlmsg_len;
    struct nlattr *attrs;
    struct nlattr *tb[3 + 1];
    int *ret = arg;
    int ack_len = sizeof(*nlh) + sizeof(int) + sizeof(*nlh);

    if (err->error > 0) {

		 /* This is illegal, per netlink(7), but not impossible (think
		 * "vendor commands"). Callers really expect negative error
		 * codes, so make that happen.
		 */
        fprintf(stderr,
                "ERROR: received positive netlink error code %d\n",
                err->error);
        *ret = -EPROTO;
    } else {
        *ret = err->error;
    }

    if (!(nlh->nlmsg_flags & 0x200))
        return NL_STOP;

    if (!(nlh->nlmsg_flags & 0x100))
        ack_len += err->msg.nlmsg_len - sizeof(*nlh);

    if (len <= ack_len)
        return NL_STOP;

    attrs = (void *)((unsigned char *)nlh + ack_len);
    len -= ack_len;

    nla_parse(tb, 3, attrs, len, NULL);
    if (tb[1]) {
        len = strnlen((char *)nla_data(tb[1]),
                      nla_len(tb[1]));
        fprintf(stderr, "kernel reports: %*s\n", len,
                (char *)nla_data(tb[1]));
    }

    return NL_STOP;
}

static int finish_handler(struct nl_msg *msg, void *arg) {
    int *ret = arg;
    *ret = 0;
    return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg) {
    int *ret = arg;
    *ret = 0;
    return NL_STOP;
}

static int no_seq_check(struct nl_msg *msg, void *arg) {
    return NL_OK;
}

static int nl80211_init(struct nl80211_state *state) {  // from iw
    int err;

    // allocate socket
    state->nl_sock = nl_socket_alloc();
    if (!state->nl_sock) {
        fprintf(stderr, "Failed to allocate netlink socket.\n");
        return -ENOMEM;
    }

    // connect a Generic Netlink socket
    if (genl_connect(state->nl_sock)) {
        fprintf(stderr, "Failed to connect to generic netlink.\n");
        err = -ENOLINK;
        goto out_handle_destroy;
    }

    // set socket buffer size
    err = nl_socket_set_buffer_size(state->nl_sock, 32 * 1024, 32 * 1024);

    if (err)
        return 1;
    /* try to set NETLINK_EXT_ACK to 1, ignoring errors */
    err = 1;
    setsockopt(nl_socket_get_fd(state->nl_sock), 270,
               1, &err, sizeof(err));

    // Resolves the Generic Netlink family name to the corresponding
    // numeric family identifier
    state->nl80211_id = genl_ctrl_resolve(state->nl_sock, "nl80211");
    if (state->nl80211_id < 0) {
        fprintf(stderr, "nl80211 not found.\n");
        err = -ENOENT;
        goto out_handle_destroy;
    }

    return 0;

out_handle_destroy:
    nl_socket_free(state->nl_sock);
    return err;
}


static int set_ftm_peer(struct nl_msg *msg, int index) {
    struct nlattr *peer = nla_nest_start(msg, index);
    if (!peer)
        goto nla_put_failure;
    uint8_t mac_addr[6] = {0x0a, 0x83, 0xa1, 0x15, 0xbf, 0x50}; // placeholder here!
    NLA_PUT(msg, NL80211_PMSR_PEER_ATTR_ADDR, 6, mac_addr);
    struct nlattr *req, *req_data, *ftm;
    req = nla_nest_start(msg, NL80211_PMSR_PEER_ATTR_REQ);
    if (!req)
        goto nla_put_failure;
    req_data = nla_nest_start(msg, NL80211_PMSR_REQ_ATTR_DATA);
    if (!req_data)
        goto nla_put_failure;
    ftm = nla_nest_start(msg, NL80211_PMSR_TYPE_FTM);
    if (!ftm)
        goto nla_put_failure;

    /*
     设置 request 的参数
     有许多参数在这里并没有设置
     参考 nl80211.h 中的 enum nl80211_peer_measurement_ftm_req
     */

    NLA_PUT_U32(msg, NL80211_PMSR_FTM_REQ_ATTR_PREAMBLE, NL80211_PREAMBLE_HT);  // required
    NLA_PUT_U8(msg, NL80211_PMSR_FTM_REQ_ATTR_NUM_FTMR_RETRIES, 5); // optional
    NLA_PUT_FLAG(msg, NL80211_PMSR_FTM_REQ_ATTR_ASAP);  // required
    nla_nest_end(msg, ftm);
    nla_nest_end(msg, req_data);
    nla_nest_end(msg, req);

    struct nlattr *chan = nla_nest_start(msg, NL80211_PMSR_PEER_ATTR_CHAN);
    if (!chan)
        goto nla_put_failure;

    NLA_PUT_U32(msg, NL80211_ATTR_CHANNEL_WIDTH,
                NL80211_CHAN_WIDTH_20);              // optional, not 20!
    NLA_PUT_U32(msg, NL80211_ATTR_WIPHY_FREQ, 2412); // required

    nla_nest_end(msg, chan);
    nla_nest_end(msg, peer);
    return 0;
nla_put_failure:
    printf("put failed!\n");
    return -1;
}

static int set_ftm_config(struct nl_msg *msg) {
    struct nlattr *pmsr = nla_nest_start(msg, NL80211_ATTR_PEER_MEASUREMENTS);
    if (!pmsr)
        return 1;
    struct nlattr *peers = nla_nest_start(msg, NL80211_PMSR_ATTR_PEERS);
    if (!peers)
        return 1;
    set_ftm_peer(msg, 1);
    nla_nest_end(msg, peers);
    nla_nest_end(msg, pmsr);
    return 0;
}

static int start_ftm(struct nl80211_state *state) {
    int err;
    struct nl_msg *msg = nlmsg_alloc();
    if (!msg) {
        printf("Fail to allocate message!");
        return 1;
    }

    genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, state->nl80211_id, 0, 0,
                NL80211_CMD_PEER_MEASUREMENT_START, 0);
    
    /*
     这里获取网卡的序号
     在 terminal 中输入 iwconfig 查询网卡名称
     */
    signed long long devidx = if_nametoindex("wlp3s0"); // placeholder here!
    if (devidx == 0) {
        printf("Fail to find device!\n");
        return 1;
    }

    NLA_PUT_U32(msg, NL80211_ATTR_IFINDEX, devidx);

    err = set_ftm_config(msg);
    if (err)
        return 1;

    struct nl_cb *cb = nl_cb_alloc(NL_CB_DEFAULT);

    if (!cb) {
        printf("Fail to allocate callback\n");
        return 1;
    }

    nl_socket_set_cb(state->nl_sock, cb);

    err = nl_send_auto(state->nl_sock, msg);
    if (err < 0)
        return 1;
    
    err = 1;
    nl_cb_err(cb, NL_CB_CUSTOM, error_handler, &err);
    nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &err);
    nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, &err);

    while (err > 0)
        nl_recvmsgs(state->nl_sock, cb);
    if (err == -1) 
        printf("Permission denied!\n");
    if (err < 0) {
        printf("Received error code %d\n", err);
        return 1;
    }
    return 0;
nla_put_failure:
    return 1;

}

static int handle_ftm_result(struct nl_msg *msg, void *arg) {
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
    if (gnlh->cmd == NL80211_CMD_PEER_MEASUREMENT_COMPLETE) {
        int * ret = arg;
        *ret = 0;
        return -1;
    }
    if (gnlh->cmd != NL80211_CMD_PEER_MEASUREMENT_RESULT)
        return -1;

    struct nlattr *tb[NL80211_ATTR_MAX + 1];
    int err;

    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);

    if (!tb[NL80211_ATTR_COOKIE]) {
		printf("Peer measurements: no cookie!\n");
		return -1;
	}

    if (!tb[NL80211_ATTR_PEER_MEASUREMENTS]) {
		printf("Peer measurements: no measurement data!\n");
		return -1;
	}

    struct nlattr *pmsr[NL80211_PMSR_ATTR_MAX + 1];
    err = nla_parse_nested(pmsr, NL80211_PMSR_ATTR_MAX,
                           tb[NL80211_ATTR_PEER_MEASUREMENTS],
                           NULL);
    if (err)
        return -1;

    if (!pmsr[NL80211_PMSR_ATTR_PEERS]) {
        printf("Peer measurements: no peer data!\n");
        return -1;
    }

    struct nlattr *peer, **resp;
    int i;
    nla_for_each_nested(peer, pmsr[NL80211_PMSR_ATTR_PEERS], i) {

        struct nlattr *peer_tb[NL80211_PMSR_PEER_ATTR_MAX + 1];
        struct nlattr *resp[NL80211_PMSR_RESP_ATTR_MAX + 1];
        struct nlattr *data[NL80211_PMSR_TYPE_MAX + 1];
        struct nlattr *ftm[NL80211_PMSR_FTM_RESP_ATTR_MAX + 1];

        err = nla_parse_nested(peer_tb, NL80211_PMSR_PEER_ATTR_MAX, peer, NULL);
        if (err) {
            printf("  Peer: failed to parse!\n");
            return 1;
        }
        if (!peer_tb[NL80211_PMSR_PEER_ATTR_ADDR]) {
            printf("  Peer: no MAC address\n");
            return 1;
        }

        if (!peer_tb[NL80211_PMSR_PEER_ATTR_RESP]) {
            printf(" no response!\n");
            return 1;
        }

        err = nla_parse_nested(resp, NL80211_PMSR_RESP_ATTR_MAX,
                               peer_tb[NL80211_PMSR_PEER_ATTR_RESP], NULL);
        if (err) {
            printf(" failed to parse response!\n");
            return 1;
        }


        err = nla_parse_nested(data, NL80211_PMSR_TYPE_MAX,
                               resp[NL80211_PMSR_RESP_ATTR_DATA], 
                               NULL);
        if (err)
            return 1;

        err = nla_parse_nested(ftm, NL80211_PMSR_FTM_RESP_ATTR_MAX,
                               data[NL80211_PMSR_TYPE_FTM], NULL);
        if (err)
            return 1;
            
        /*
         获取测距结果
         参考 nl80211.h 中的 enum nl80211_peer_measurement_ftm_resp
         */
        int64_t dist = 0;
        int64_t rtt = 0;

        if (ftm[NL80211_PMSR_FTM_RESP_ATTR_DIST_AVG])
            dist = nla_get_s64(ftm[NL80211_PMSR_FTM_RESP_ATTR_DIST_AVG]);

        if (ftm[NL80211_PMSR_FTM_RESP_ATTR_RTT_AVG])
            rtt = nla_get_s64(ftm[NL80211_PMSR_FTM_RESP_ATTR_RTT_AVG]);
        
        double dist_result = 0;
        if (dist) {
            dist_result = (double)dist / 1000;
        } else if (rtt) {
            dist_result = (double)rtt * SOL / 1000000000000;
        }
        printf("%-12s%6.3f m\n", "distance: ", dist_result);
    };
    return 0;
}

static int listen_ftm_result(struct nl80211_state *state) {
    struct nl_cb *cb = nl_cb_alloc(NL_CB_DEFAULT); // use NL_CB_DEBUG when debugging
    if (!cb)
        return 1;

    nl_socket_set_cb(state->nl_sock, cb);

    int status = 1;

    nl_cb_set(cb, NL_CB_SEQ_CHECK, NL_CB_CUSTOM, no_seq_check, NULL);
    nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, handle_ftm_result, &status);

    while (status)
        nl_recvmsgs(state->nl_sock, cb);
    return 0;
nla_put_failure:
    return 1;
}

int main(int argc, int** argv) {
    struct nl80211_state nlstate;
    int err = nl80211_init(&nlstate);
    if (err) {
    	printf("Fail to allocate socket!\n");
        return 1;
    }

    int count = 0;
    while (count < 100) {
        err = start_ftm(&nlstate);
        if (err) {
            printf("Fail to start ftm!\n");
            return 1;
        }

        err = listen_ftm_result(&nlstate);
        if (err) {
            printf("Fail to listen!\n");
            return 1;
        }
        count++;
    }
    return 0;
}
