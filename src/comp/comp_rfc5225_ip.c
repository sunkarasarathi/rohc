/*
 * Copyright 2018 Viveris Technologies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file   comp_rfc5225_ip.c
 * @brief  ROHC compression context for the ROHCv2 IP-only profile
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 */

#include "rohc_comp_internals.h"
#include "rohc_traces.h"
#include "rohc_traces_internal.h"
#include "rohc_debug.h"
#include "rohc_bit_ops.h"
#include "protocols/ip_numbers.h"
#include "protocols/ip.h"
#include "protocols/rfc5225.h"
#include "schemes/cid.h"
#include "schemes/ipv6_exts.h"
#include "schemes/ip_ctxt.h"
#include "schemes/comp_wlsb.h"
#include "schemes/ip_id_offset.h"
#include "crc.h"

#include <assert.h>


/**
 * @brief Define the RFC5225-specific temporary variables in the profile
 *        compression context
 */
struct comp_rfc5225_tmp_variables
{
	/** Whether at least one of the DF fields changed */
	bool df_changed;
	/** Whether the behavior of at least one of the IP-ID fields changed */
	bool ip_id_behavior_changed;
	/* Whether at least one TOS/TC or TTL/HL changed in all outer IP headers */
	bool outer_ip_flag;
	/** Whether the innermost TOS/TC or TTL/HL changed in the innermost IP header */
	bool innermost_ip_flag;
	/** The new innermost IP-ID value */
	uint16_t innermost_ip_id;
	/** The new innermost IP-ID / SN delta (with bits swapped if necessary) */
	uint16_t innermost_ip_id_offset;
};


/** Define the ROHCv2 IP-only part of the profile compression context */
struct rohc_comp_rfc5225_ip_ctxt
{
	uint16_t msn;  /**< The Master Sequence Number (MSN) */
	struct c_wlsb msn_wlsb;    /**< The W-LSB encoding context for MSN */

	/** The W-LSB encoding context for innermost IP-ID offset */
	struct c_wlsb innermost_ip_id_offset_wlsb;

	ip_context_t ip_contexts[ROHC_MAX_IP_HDRS];
	size_t ip_contexts_nr;

	struct comp_rfc5225_tmp_variables tmp;
};


/*
 * Prototypes of private functions
 */

/* create/destroy context */
static bool rohc_comp_rfc5225_ip_create(struct rohc_comp_ctxt *const context,
                                        const struct net_pkt *const packet)
	__attribute__((warn_unused_result, nonnull(1, 2)));
static void rohc_comp_rfc5225_ip_destroy(struct rohc_comp_ctxt *const context)
	__attribute__((nonnull(1)));
static bool rohc_comp_rfc5225_ip_check_profile(const struct rohc_comp *const comp,
                                               const struct net_pkt *const packet)
	__attribute__((warn_unused_result, nonnull(1, 2)));

/* check whether a packet belongs to a context */
static bool rohc_comp_rfc5225_ip_check_context(const struct rohc_comp_ctxt *const context,
                                               const struct net_pkt *const packet,
                                               size_t *const cr_score)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));

/* encode ROHCv2 IP-only packets */
static int rohc_comp_rfc5225_ip_encode(struct rohc_comp_ctxt *const context,
                                       const struct net_pkt *const uncomp_pkt,
                                       uint8_t *const rohc_pkt,
                                       const size_t rohc_pkt_max_len,
                                       rohc_packet_t *const packet_type,
                                       size_t *const payload_offset)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 5, 6)));

static int rohc_comp_rfc5225_ip_code_IR_pkt(const struct rohc_comp_ctxt *const ctxt,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));

static int rohc_comp_rfc5225_ip_code_CO_pkt(const struct rohc_comp_ctxt *const context,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len,
                                            const rohc_packet_t packet_type,
                                            const size_t payload_offset)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));

static int rohc_comp_rfc5225_ip_build_pt_0_crc3_pkt(const struct rohc_comp_ctxt *const context,
                                                    const uint8_t crc,
                                                    uint8_t *const rohc_data,
                                                    const size_t rohc_max_len)
	__attribute__((nonnull(1, 3), warn_unused_result));

static int rohc_comp_rfc5225_ip_build_pt_0_crc7_pkt(const struct rohc_comp_ctxt *const context,
                                                    const uint8_t crc,
                                                    uint8_t *const rohc_data,
                                                    const size_t rohc_max_len)
	__attribute__((nonnull(1, 3), warn_unused_result));

static int rohc_comp_rfc5225_ip_build_pt_1_seq_id_pkt(const struct rohc_comp_ctxt *const context,
                                                      const uint8_t crc,
                                                      uint8_t *const rohc_data,
                                                      const size_t rohc_max_len)
	__attribute__((nonnull(1, 3), warn_unused_result));

static int rohc_comp_rfc5225_ip_build_pt_2_seq_id_pkt(const struct rohc_comp_ctxt *const context,
                                                      const uint8_t crc,
                                                      uint8_t *const rohc_data,
                                                      const size_t rohc_max_len)
	__attribute__((nonnull(1, 3), warn_unused_result));

/* static chain */
static int rohc_comp_rfc5225_ip_static_chain(const struct rohc_comp_ctxt *const ctxt,
                                             const struct ip_packet *const ip,
                                             uint8_t *const rohc_pkt,
                                             const size_t rohc_pkt_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));
static int rohc_comp_rfc5225_ip_static_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                                 const struct ipv4_hdr *const ipv4,
                                                 const bool is_innermost,
                                                 uint8_t *const rohc_data,
                                                 const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));
static int rohc_comp_rfc5225_ip_static_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                                 const struct ipv6_hdr *const ipv6,
                                                 const bool is_innermost,
                                                 uint8_t *const rohc_data,
                                                 const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 4)));

/* dynamic chain */
static int rohc_comp_rfc5225_ip_dyn_chain(const struct rohc_comp_ctxt *const ctxt,
                                          const struct ip_packet *const ip,
                                          uint8_t *const rohc_pkt,
                                          const size_t rohc_pkt_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3)));
static int rohc_comp_rfc5225_ip_dyn_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                              const ip_context_t *const ip_ctxt,
                                              const struct ipv4_hdr *const ipv4,
                                              const bool is_innermost,
                                              uint8_t *const rohc_data,
                                              const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));
static int rohc_comp_rfc5225_ip_dyn_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                              const ip_context_t *const ip_ctxt,
                                              const struct ipv6_hdr *const ipv6,
                                              const bool is_innermost,
                                              uint8_t *const rohc_data,
                                              const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));

/* irregular chain */
static int rohc_comp_rfc5225_ip_irreg_chain(const struct rohc_comp_ctxt *const ctxt,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len)
        __attribute__((warn_unused_result, nonnull(1, 2, 3)));
static int rohc_comp_rfc5225_ip_irreg_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                                const ip_context_t *const ip_ctxt,
                                                const struct ipv4_hdr *const ipv4,
                                                const bool is_innermost,
                                                uint8_t *const rohc_data,
                                                const size_t rohc_max_len)
	__attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));
static int rohc_comp_rfc5225_ip_irreg_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                                const ip_context_t *const ip_ctxt,
                                                const struct ipv6_hdr *const ipv6,
                                                const bool is_innermost,
                                                uint8_t *const rohc_data,
                                                const size_t rohc_max_len)
        __attribute__((warn_unused_result, nonnull(1, 2, 3, 5)));

/* deliver feedbacks */
static bool rohc_comp_rfc5225_ip_feedback(struct rohc_comp_ctxt *const context,
                                          const enum rohc_feedback_type feedback_type,
                                          const uint8_t *const packet,
                                          const size_t packet_len,
                                          const uint8_t *const feedback_data,
                                          const size_t feedback_data_len)
	__attribute__((warn_unused_result, nonnull(1, 3, 5)));

/* mode and state transitions */
static void rohc_comp_rfc5225_ip_decide_state(struct rohc_comp_ctxt *const context,
                                              const struct rohc_ts pkt_time)
	__attribute__((nonnull(1)));

/* decide packet */
static rohc_packet_t rohc_comp_rfc5225_ip_decide_pkt(struct rohc_comp_ctxt *const context)
	__attribute__((warn_unused_result, nonnull(1)));

static rohc_packet_t rohc_comp_rfc5225_ip_decide_FO_pkt(const struct rohc_comp_ctxt *const ctxt)
	__attribute__((warn_unused_result, nonnull(1)));

static rohc_packet_t rohc_comp_rfc5225_ip_decide_SO_pkt(const struct rohc_comp_ctxt *const ctxt)
	__attribute__((warn_unused_result, nonnull(1)));

static rohc_packet_t rohc_comp_rfc5225_ip_decide_FO_SO_pkt(const struct rohc_comp_ctxt *const ctxt,
                                                           const bool crc7_at_least)
	__attribute__((warn_unused_result, nonnull(1)));

static bool rohc_comp_rfc5225_is_msn_lsb_possible(const struct c_wlsb *const wlsb,
                                                  const uint16_t value,
                                                  const rohc_reordering_offset_t reorder_ratio,
                                                  const size_t k)
	__attribute__((warn_unused_result, nonnull(1)));

static bool rohc_comp_rfc5225_is_ipid_sequential(const rohc_ip_id_behavior_t behavior)
	__attribute__((warn_unused_result, const));

static bool rohc_comp_rfc5225_is_seq_ipid_inferred(const ip_context_t *const ip_ctxt,
                                                   const uint16_t new_ip_id)
	__attribute__((warn_unused_result, nonnull(1)));


/*
 * Definitions of private functions
 */


/**
 * @brief Create a new ROHCv2 IP-only context and initialize it thanks
 *        to the given uncompressed packet
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context  The compression context
 * @param packet   The packet given to initialize the new context
 * @return         true if successful, false otherwise
 */
static bool rohc_comp_rfc5225_ip_create(struct rohc_comp_ctxt *const context,
                                        const struct net_pkt *const packet)
{
	const struct rohc_comp *const comp = context->compressor;
	struct rohc_comp_rfc5225_ip_ctxt *rfc5225_ctxt;
	const uint8_t *remain_data = packet->outer_ip.data;
	size_t remain_len = packet->outer_ip.size;
	uint8_t proto;

	/* create the ROHCv2 IP-only part of the profile context */
	rfc5225_ctxt = calloc(1, sizeof(struct rohc_comp_rfc5225_ip_ctxt));
	if(rfc5225_ctxt == NULL)
	{
		rohc_error(comp, ROHC_TRACE_COMP, context->profile->id,
		           "no memory for the ROHCv2 IP-only part of the profile context");
		goto error;
	}
	context->specific = rfc5225_ctxt;

	/* create contexts for IP headers and their extensions */
	rfc5225_ctxt->ip_contexts_nr = 0;
	do
	{
		const struct ip_hdr *const ip = (struct ip_hdr *) remain_data;
		ip_context_t *const ip_context =
			&(rfc5225_ctxt->ip_contexts[rfc5225_ctxt->ip_contexts_nr]);

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(context, "found IPv%d", ip->version);
		ip_context->version = ip->version;
		ip_context->ctxt.vx.version = ip->version;

		switch(ip->version)
		{
			case IPV4:
			{
				const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;

				assert(remain_len >= sizeof(struct ipv4_hdr));
				proto = ipv4->protocol;

				ip_context->ctxt.v4.last_ip_id = rohc_ntoh16(ipv4->id);
				rohc_comp_debug(context, "IP-ID 0x%04x", ip_context->ctxt.v4.last_ip_id);
				ip_context->ctxt.v4.last_ip_id_behavior = ROHC_IP_ID_BEHAVIOR_SEQ;
				ip_context->ctxt.v4.ip_id_behavior = ROHC_IP_ID_BEHAVIOR_SEQ;
				ip_context->ctxt.v4.protocol = proto;
				ip_context->ctxt.v4.tos = ipv4->tos;
				ip_context->ctxt.v4.df = ipv4->df;
				ip_context->ctxt.v4.ttl = ipv4->ttl;
				ip_context->ctxt.v4.src_addr = ipv4->saddr;
				ip_context->ctxt.v4.dst_addr = ipv4->daddr;

				remain_data += sizeof(struct ipv4_hdr);
				remain_len -= sizeof(struct ipv4_hdr);
				break;
			}
			case IPV6:
			{
				const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

				assert(remain_len >= sizeof(struct ipv6_hdr));
				proto = ipv6->nh;

				ip_context->ctxt.v6.ip_id_behavior = ROHC_IP_ID_BEHAVIOR_ZERO;
				ip_context->ctxt.v6.tc = remain_data[1];
				ip_context->ctxt.v6.hopl = ipv6->hl;
				ip_context->ctxt.v6.flow_label = ipv6_get_flow_label(ipv6);
				memcpy(ip_context->ctxt.v6.src_addr, &ipv6->saddr,
				       sizeof(struct ipv6_addr));
				memcpy(ip_context->ctxt.v6.dest_addr, &ipv6->daddr,
				       sizeof(struct ipv6_addr));

				remain_data += sizeof(struct ipv6_hdr);
				remain_len -= sizeof(struct ipv6_hdr);

				/* TODO: handle IPv6 extension headers */
				assert(rohc_is_ipv6_opt(proto) == false);

				ip_context->ctxt.v6.next_header = proto;
				break;
			}
			default:
			{
				goto free_context;
			}
		}

		rfc5225_ctxt->ip_contexts_nr++;
	}
	while(rohc_is_tunneling(proto) && rfc5225_ctxt->ip_contexts_nr < ROHC_MAX_IP_HDRS);

	/* profile cannot handle the packet if it bypasses internal limit of IP headers */
	if(rohc_is_tunneling(proto))
	{
		rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
		           "too many IP headers for RFC5225 IP-only profile (%u headers max)",
		           ROHC_MAX_IP_HDRS);
		goto free_context;
	}

	/* MSN */
	wlsb_init(&rfc5225_ctxt->msn_wlsb, 16, comp->wlsb_window_width, ROHC_LSB_SHIFT_VAR);
	/* innermost IP-ID offset */
	wlsb_init(&rfc5225_ctxt->innermost_ip_id_offset_wlsb, 16,
	          comp->wlsb_window_width, ROHC_LSB_SHIFT_VAR);

	/* init the Master Sequence Number to a random value */
	rfc5225_ctxt->msn = comp->random_cb(comp, comp->random_cb_ctxt) & 0xffff;
	rohc_comp_debug(context, "MSN = 0x%04x / %u", rfc5225_ctxt->msn, rfc5225_ctxt->msn);

	return true;

free_context:
	free(rfc5225_ctxt);
error:
	return false;
}


/**
 * @brief Destroy the ROHCv2 IP-only context
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The ROHCv2 IP-only compression context to destroy
 */
static void rohc_comp_rfc5225_ip_destroy(struct rohc_comp_ctxt *const context)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;

	free(rfc5225_ctxt);
}


/**
 * @brief Check if the given packet corresponds to the ROHCv2 IP-only profile
 *
 * Conditions are:
 *  \li the versions of the IP headers are all 4 or 6
 *  \li none of the IP headers is an IP fragment
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param comp    The ROHC compressor
 * @param packet  The packet to check
 * @return        Whether the packet corresponds to the profile:
 *                  \li true if the packet corresponds to the profile,
 *                  \li false if the packet does not correspond to
 *                      the profile

 */
static bool rohc_comp_rfc5225_ip_check_profile(const struct rohc_comp *const comp,
                                               const struct net_pkt *const packet)
{
	/* TODO: should avoid code duplication by using net_pkt as
	 * rohc_comp_rfc3095_check_profile() does */
	const uint8_t *remain_data;
	size_t remain_len;
	size_t ip_hdrs_nr;
	uint8_t next_proto;

	remain_data = packet->outer_ip.data;
	remain_len = packet->outer_ip.size;

	/* check that the the versions of IP headers are 4 or 6 and that IP headers
	 * are not IP fragments */
	ip_hdrs_nr = 0;
	do
	{
		const struct ip_hdr *const ip = (struct ip_hdr *) remain_data;

		/* check minimal length for IP version */
		if(remain_len < sizeof(struct ip_hdr))
		{
			rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
			           "failed to determine the version of IP header #%zu",
			           ip_hdrs_nr + 1);
			goto bad_profile;
		}

		if(ip->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;
			const size_t ipv4_min_words_nr = sizeof(struct ipv4_hdr) / sizeof(uint32_t);

			rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL, "found IPv4");
			if(remain_len < sizeof(struct ipv4_hdr))
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "uncompressed packet too short for IP header #%zu",
				           ip_hdrs_nr + 1);
				goto bad_profile;
			}

			/* IPv4 options are not supported by the ROHCv2 IP-only profile */
			if(ipv4->ihl != ipv4_min_words_nr)
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not supported by the profile: "
				           "IP options are not accepted", ip_hdrs_nr + 1);
				goto bad_profile;
			}

			/* IPv4 total length shall be correct */
			if(rohc_ntoh16(ipv4->tot_len) != remain_len)
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not supported by the profile: total "
				           "length is %u while it shall be %zu", ip_hdrs_nr + 1,
				           rohc_ntoh16(ipv4->tot_len), remain_len);
				goto bad_profile;
			}

			/* check if the IPv4 header is a fragment */
			if(ipv4_is_fragment(ipv4))
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is fragmented", ip_hdrs_nr + 1);
				goto bad_profile;
			}

			/* check if the checksum of the IPv4 header is correct */
			if((comp->features & ROHC_COMP_FEATURE_NO_IP_CHECKSUMS) == 0 &&
			   ip_fast_csum(remain_data, ipv4_min_words_nr) != 0)
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not correct (bad checksum)",
				           ip_hdrs_nr + 1);
				goto bad_profile;
			}

			next_proto = ipv4->protocol;
			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;
			size_t ipv6_exts_len;

			rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL, "found IPv6");
			if(remain_len < sizeof(struct ipv6_hdr))
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "uncompressed packet too short for IP header #%zu",
				           ip_hdrs_nr + 1);
				goto bad_profile;
			}
			next_proto = ipv6->nh;
			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* payload length shall be correct */
			if(rohc_ntoh16(ipv6->plen) != remain_len)
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not supported by the profile: payload "
				           "length is %u while it shall be %zu", ip_hdrs_nr + 1,
				           rohc_ntoh16(ipv6->plen), remain_len);
				goto bad_profile;
			}

			/* reject packets with malformed IPv6 extension headers or IPv6
			 * extension headers that are not compatible with the ROHCv2 IP-only
			 * profile */
			if(!rohc_comp_ipv6_exts_are_acceptable(comp, &next_proto,
			                                       remain_data, remain_len,
			                                       &ipv6_exts_len))
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not supported by the profile: "
				           "malformed or incompatible IPv6 extension headers "
				           "detected", ip_hdrs_nr + 1);
				goto bad_profile;
			}
			/* TODO: handle IPv6 extension headers */
			if(ipv6_exts_len > 0)
			{
				rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
				           "IP packet #%zu is not supported by the profile: "
				           "IPv6 extension headers detected", ip_hdrs_nr + 1);
				goto bad_profile;
			}
			remain_data += ipv6_exts_len;
			remain_len -= ipv6_exts_len;
		}
		else
		{
			rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
			           "unsupported version %u for header #%zu",
			           ip->version, ip_hdrs_nr + 1);
			goto bad_profile;
		}
		ip_hdrs_nr++;
	}
	while(rohc_is_tunneling(next_proto) && ip_hdrs_nr < ROHC_MAX_IP_HDRS);

	/* profile cannot handle the packet if it bypasses internal limit of IP headers */
	if(rohc_is_tunneling(next_proto))
	{
		rohc_debug(comp, ROHC_TRACE_COMP, ROHC_PROFILE_GENERAL,
		           "too many IP headers for ROHCv2 IP-only profile (%u headers max)",
		           ROHC_MAX_IP_HDRS);
		goto bad_profile;
	}

	return true;

bad_profile:
	return false;
}


/**
 * @brief Check if the IP packet belongs to the given ROHCv2 IP-only context
 *
 * Conditions are:
 *  - the number of IP headers must be the same as in context
 *  - IP version of all the IP headers must be the same as in context
 *  - IP packets must not be fragmented
 *  - the source and destination addresses of the two IP headers must match
 *    the ones in the context
 *  - IPv6 only: the Flow Label of all the IP headers must match the ones the
 *    context
 *

 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context        The compression context
 * @param packet         The IP packet to check
 * @param[out] cr_score  The score of the context for Context Replication (CR)
 * @return               true if the IP packet belongs to the context
 *                       false if it does not belong to the context
 *
 * @todo TODO: the code that parses IP headers in IP/UDP/RTP profiles could
 *             probably be re-used (and maybe enhanced if needed)
 */
static bool rohc_comp_rfc5225_ip_check_context(const struct rohc_comp_ctxt *const context,
                                               const struct net_pkt *const packet,
                                               size_t *const cr_score)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;
	const uint8_t *remain_data = packet->outer_ip.data;
	size_t remain_len = packet->outer_ip.size;
	size_t ip_hdr_pos;
	uint8_t next_proto = ROHC_IPPROTO_IPIP;

	*cr_score = 0; /* Context Replication is not defined for ROHCv2 IP-only profile */

	/* parse the IP headers (lengths already checked while checking profile) */
	for(ip_hdr_pos = 0;
	    ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr && rohc_is_tunneling(next_proto);
	    ip_hdr_pos++)
	{
		const struct ip_hdr *const ip = (struct ip_hdr *) remain_data;
		const ip_context_t *const ip_context = &(rfc5225_ctxt->ip_contexts[ip_hdr_pos]);

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(context, "found IPv%d", ip->version);
		if(ip->version != ip_context->version)
		{
			rohc_comp_debug(context, "  not same IP version");
			goto bad_context;
		}

		if(ip->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv4_hdr));

			/* check source address */
			if(ipv4->saddr != ip_context->ctxt.v4.src_addr)
			{
				rohc_comp_debug(context, "  not same IPv4 source addresses");
				goto bad_context;
			}
			rohc_comp_debug(context, "  same IPv4 source addresses");

			/* check destination address */
			if(ipv4->daddr != ip_context->ctxt.v4.dst_addr)
			{
				rohc_comp_debug(context, "  not same IPv4 destination addresses");
				goto bad_context;
			}
			rohc_comp_debug(context, "  same IPv4 destination addresses");

			/* check transport protocol */
			next_proto = ipv4->protocol;
			if(next_proto != ip_context->ctxt.v4.protocol)
			{
				rohc_comp_debug(context, "  IPv4 not same protocol");
				goto bad_context;
			}
			rohc_comp_debug(context, "  IPv4 same protocol %d", next_proto);

			/* skip IPv4 header */
			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv6_hdr));

			/* check source address */
			if(memcmp(&ipv6->saddr, ip_context->ctxt.v6.src_addr,
			          sizeof(struct ipv6_addr)) != 0)
			{
				rohc_comp_debug(context, "  not same IPv6 source addresses");
				goto bad_context;
			}
			rohc_comp_debug(context, "  same IPv6 source addresses");

			/* check destination address */
			if(memcmp(&ipv6->daddr, ip_context->ctxt.v6.dest_addr,
			          sizeof(struct ipv6_addr)) != 0)
			{
				rohc_comp_debug(context, "  not same IPv6 destination addresses");
				goto bad_context;
			}
			rohc_comp_debug(context, "  same IPv6 destination addresses");

			/* check Flow Label */
			if(ipv6_get_flow_label(ipv6) != ip_context->ctxt.v6.flow_label)
			{
				rohc_comp_debug(context, "  not same IPv6 flow label");
				goto bad_context;
			}
			rohc_comp_debug(context, "  same IPv6 flow label");

			/* skip IPv6 base header */
			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* find transport header/protocol, skip any IPv6 extension headers */
			next_proto = ipv6->nh;
			/* TODO: handle IPv6 extension headers */
			assert(rohc_is_ipv6_opt(next_proto) == false);

			/* check transport header protocol */
			if(next_proto != ip_context->ctxt.v6.next_header)
			{
				rohc_comp_debug(context, "  IPv6 not same protocol %u", next_proto);
				goto bad_context;
			}
			rohc_comp_debug(context, "  IPv6 same protocol %u", next_proto);
		}
		else
		{
			rohc_comp_warn(context, "unsupported version %u for header #%zu",
			               ip->version, ip_hdr_pos + 1);
			assert(0);
			goto bad_context;
		}
	}

	if(ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr)
	{
		rohc_comp_debug(context, "  less IP headers than context");
		goto bad_context;
	}

	if(rohc_is_tunneling(next_proto))
	{
		rohc_comp_debug(context, "  more IP headers than context");
		goto bad_context;
	}

	return true;

bad_context:
	return false;
}


/**
 * @brief Encode an uncompressed packet according to a pattern decided by
 *        several different factors
 *
 * 1. Decide state\n
 * 2. Decide which packet type to send.\n
 * 3. Code packet\n
 * 4. Update context\n
 * \n
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context           The compression context
 * @param uncomp_pkt        The uncompressed packet to encode
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param packet_type       OUT: The type of ROHC packet that is created
 * @param payload_offset    OUT: The offset for the payload in the uncompressed
 *                          packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_encode(struct rohc_comp_ctxt *const context,
                                       const struct net_pkt *const uncomp_pkt,
                                       uint8_t *const rohc_pkt,
                                       const size_t rohc_pkt_max_len,
                                       rohc_packet_t *const packet_type,
                                       size_t *const payload_offset)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;

	const uint8_t *remain_data = uncomp_pkt->data;
	size_t remain_len = uncomp_pkt->len;

	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;

	size_t ip_hdr_pos;
	size_t rohc_len;
	int ret;

	*packet_type = ROHC_PACKET_UNKNOWN;
	*payload_offset = 0;

	/* compute or find the new SN */
	rfc5225_ctxt->msn = (rfc5225_ctxt->msn + 1) % 0xffff;
	rohc_comp_debug(context, "MSN = 0x%04x / %u", rfc5225_ctxt->msn, rfc5225_ctxt->msn);

	/* STEP 0: detect changes */
	/* parse the IP headers and their extension headers to identify the length
	 * of the uncompressed headers */
	/* TODO: find a better to detect the uncompressed header length */
	rohc_comp_debug(context, "parse the %zu-byte IP packet", remain_len);
	assert(rfc5225_ctxt->ip_contexts_nr > 0);
	rfc5225_ctxt->tmp.outer_ip_flag = false;
	rfc5225_ctxt->tmp.innermost_ip_flag = false;
	rfc5225_ctxt->tmp.df_changed = false;
	rfc5225_ctxt->tmp.ip_id_behavior_changed = false;
	for(ip_hdr_pos = 0; ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr; ip_hdr_pos++)
	{
		const struct ip_hdr *const ip_hdr = (struct ip_hdr *) remain_data;
		ip_context_t *const ip_context = &(rfc5225_ctxt->ip_contexts[ip_hdr_pos]);
		const bool is_innermost =
			!!(ip_hdr_pos == (rfc5225_ctxt->ip_contexts_nr - 1));
		uint8_t protocol;

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(context, "  found %s IPv%d header",
		                is_innermost ? "innermost" : "outer", ip_hdr->version);

		if(ip_hdr->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;
			size_t ipv4_hdr_len;

			assert(remain_len >= sizeof(struct ipv4_hdr));

			protocol = ipv4->protocol;
			if(is_innermost)
			{
				if(ip_context->ctxt.vx.tos_tc != ipv4->tos ||
				   ip_context->ctxt.vx.ttl_hopl != ipv4->ttl)
				{
					rohc_comp_debug(context, "    TOS (%02x -> %02x) or TTL (%u -> %u) "
					                "changed", ip_context->ctxt.vx.tos_tc, ipv4->tos,
										 ip_context->ctxt.vx.ttl_hopl, ipv4->ttl);
					rfc5225_ctxt->tmp.innermost_ip_flag = true;
				}
			}
			else
			{
				if(ip_context->ctxt.vx.tos_tc != ipv4->tos ||
				   ip_context->ctxt.vx.ttl_hopl != ipv4->ttl)
				{
					rohc_comp_debug(context, "    TOS (%02x -> %02x) or TTL (%u -> %u) "
					                "changed", ip_context->ctxt.vx.tos_tc, ipv4->tos,
										 ip_context->ctxt.vx.ttl_hopl, ipv4->ttl);
					rfc5225_ctxt->tmp.outer_ip_flag = true;
				}
			}
			ipv4_hdr_len = ipv4->ihl * sizeof(uint32_t);

			/* IPv4 DF changed? */
			if(ip_context->ctxt.v4.df != ipv4->df)
			{
				rfc5225_ctxt->tmp.df_changed = true;
			}

			/* determine the IP-ID behavior of the IPv4 header */
			{
				const uint16_t ip_id = rohc_ntoh16(ipv4->id);
				const uint16_t last_ip_id = ip_context->ctxt.v4.last_ip_id;
				const rohc_ip_id_behavior_t last_ip_id_behavior =
					ip_context->ctxt.v4.ip_id_behavior;
				rohc_ip_id_behavior_t ip_id_behavior;

				rohc_comp_debug(context, "IP-ID behaved as %s",
				                rohc_ip_id_behavior_get_descr(last_ip_id_behavior));
				rohc_comp_debug(context, "IP-ID = 0x%04x -> 0x%04x", last_ip_id, ip_id);

				if(context->num_sent_packets == 0)
				{
					/* first packet, be optimistic: choose sequential behavior */
					ip_id_behavior = ROHC_IP_ID_BEHAVIOR_SEQ;
				}
				else
				{
					ip_id_behavior =
						rohc_comp_detect_ip_id_behavior(last_ip_id, ip_id, 19);

					/* no sequential behavior for outer IP headers */
					if(!is_innermost && ip_id_behavior <= ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
					{
						ip_id_behavior = ROHC_IP_ID_BEHAVIOR_RAND;
					}
				}
				ip_context->ctxt.v4.ip_id_behavior = ip_id_behavior;
				rohc_comp_debug(context, "IP-ID now behaves as %s",
				                rohc_ip_id_behavior_get_descr(ip_id_behavior));
				if(last_ip_id_behavior != ip_id_behavior)
				{
					rfc5225_ctxt->tmp.ip_id_behavior_changed = true;
				}

				/* compute the new IP-ID / SN offset of the innermost IP header */
				if(is_innermost)
				{
					rfc5225_ctxt->tmp.innermost_ip_id = ip_id;
					if(ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
					{
						/* specific case of IP-ID delta for sequential swapped behavior */
						rfc5225_ctxt->tmp.innermost_ip_id_offset =
							swab16(ip_id) - rfc5225_ctxt->msn;
					}
					else
					{
						/* compute delta the same way for sequential, zero or random: it is
						 * important to always compute the IP-ID delta and record it in W-LSB,
						 * so that the IP-ID deltas of next packets may be correctly encoded */
						rfc5225_ctxt->tmp.innermost_ip_id_offset =
							ip_id - rfc5225_ctxt->msn;
					}
					rohc_comp_debug(context, "new IP-ID offset = 0x%x / %u",
					                rfc5225_ctxt->tmp.innermost_ip_id_offset,
					                rfc5225_ctxt->tmp.innermost_ip_id_offset);
				}
			}

			/* skip IPv4 header */
			rohc_comp_debug(context, "skip %zu-byte IPv4 header with "
			                "Protocol 0x%02x", ipv4_hdr_len, protocol);
			remain_data += ipv4_hdr_len;
			remain_len -= ipv4_hdr_len;
			*payload_offset += ipv4_hdr_len;
		}
		else if(ip_hdr->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv6_hdr));

			protocol = ipv6->nh;
			if(is_innermost)
			{
				if(ip_context->ctxt.vx.tos_tc != ipv6_get_tc(ipv6) ||
				   ip_context->ctxt.vx.ttl_hopl != ipv6->hl)
				{
					rohc_comp_debug(context, "    TC (%02x -> %02x) or HL (%u -> %u) "
					                "changed", ip_context->ctxt.vx.tos_tc, ipv6_get_tc(ipv6),
										 ip_context->ctxt.vx.ttl_hopl, ipv6->hl);
					rfc5225_ctxt->tmp.innermost_ip_flag = true;
				}
			}
			else if(!rfc5225_ctxt->tmp.outer_ip_flag)
			{
				if(ip_context->ctxt.vx.tos_tc != ipv6_get_tc(ipv6) ||
				   ip_context->ctxt.vx.ttl_hopl != ipv6->hl)
				{
					rohc_comp_debug(context, "    TC (%02x -> %02x) or HL (%u -> %u) "
					                "changed", ip_context->ctxt.vx.tos_tc, ipv6_get_tc(ipv6),
										 ip_context->ctxt.vx.ttl_hopl, ipv6->hl);
					rfc5225_ctxt->tmp.outer_ip_flag = true;
				}
			}

			/* skip IPv6 header */
			rohc_comp_debug(context, "skip %zu-byte IPv6 header with Next Header "
			                "0x%02x", sizeof(struct ipv6_hdr), protocol);
			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);
			*payload_offset += sizeof(struct ipv6_hdr);

			/* TODO: handle IPv6 extension headers */
			assert(rohc_is_ipv6_opt(protocol) == false);
		}
		else
		{
			rohc_comp_warn(context, "unexpected IP version %u", ip_hdr->version);
			assert(0);
			goto error;
		}
	}

	/* STEP 1: decide state */
	rohc_comp_rfc5225_ip_decide_state(context, uncomp_pkt->time);

	/* STEP 2: decide packet type */
	*packet_type = rohc_comp_rfc5225_ip_decide_pkt(context);

	/* the outer_ip_flag may be set to 1 only for co_common */
	if(rfc5225_ctxt->tmp.outer_ip_flag && (*packet_type) != ROHC_PACKET_CO_COMMON)
	{
		rfc5225_ctxt->tmp.outer_ip_flag = false;
	}

	/* STEP 3: code packet */
	if((*packet_type) == ROHC_PACKET_IR)
	{
		ret = rohc_comp_rfc5225_ip_code_IR_pkt(context, &uncomp_pkt->outer_ip,
		                                       rohc_remain_data, rohc_remain_len);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build IR packet");
			goto error;
		}
		rohc_len = ret;
	}
	else /* CO packets */
	{

		ret = rohc_comp_rfc5225_ip_code_CO_pkt(context, &uncomp_pkt->outer_ip,
		                                       rohc_remain_data, rohc_remain_len,
		                                       *packet_type, *payload_offset);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build CO packet");
			goto error;
		}
		rohc_len = ret;
	}

	rohc_comp_dump_buf(context, "current ROHC packet", rohc_pkt, rohc_len);
	rohc_comp_debug(context, "payload_offset = %zu", *payload_offset);

	/* STEP 4: update context with new values (done at the very end to avoid
	 * wrongly updating the context in case of compression failure) */
	rohc_comp_debug(context, "update context:");
	/* add the new MSN to the W-LSB encoding object */
	c_add_wlsb(&rfc5225_ctxt->msn_wlsb, rfc5225_ctxt->msn, rfc5225_ctxt->msn);
	/* update context for all IP headers */
	remain_data = uncomp_pkt->data;
	remain_len = uncomp_pkt->len;
	for(ip_hdr_pos = 0; ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr; ip_hdr_pos++)
	{
		const struct ip_hdr *const ip_hdr = (struct ip_hdr *) remain_data;
		ip_context_t *const ip_ctxt = &(rfc5225_ctxt->ip_contexts[ip_hdr_pos]);

		if(ip_hdr->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) ip_hdr;
			ip_ctxt->ctxt.v4.last_ip_id_behavior = ip_ctxt->ctxt.v4.ip_id_behavior;
			ip_ctxt->ctxt.v4.last_ip_id = rohc_ntoh16(ipv4->id);
			/* add the new IP-ID offset to the W-LSB encoding object */
			c_add_wlsb(&rfc5225_ctxt->innermost_ip_id_offset_wlsb, rfc5225_ctxt->msn,
			           rfc5225_ctxt->tmp.innermost_ip_id_offset);
			ip_ctxt->ctxt.v4.df = ipv4->df;
			ip_ctxt->ctxt.vx.tos_tc = ipv4->tos;
			ip_ctxt->ctxt.vx.ttl_hopl = ipv4->ttl;
			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip_hdr->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) ip_hdr;
			ip_ctxt->ctxt.vx.tos_tc = ipv6_get_tc(ipv6);
			ip_ctxt->ctxt.vx.ttl_hopl = ipv6->hl;
			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* TODO: handle IPv6 extension headers */
		}
		else
		{
			rohc_comp_warn(context, "unexpected IP version %u", ip_hdr->version);
			assert(0);
			goto error;
		}
	}

	return rohc_len;

error:
	return -1;
}


/**
 * @brief Update the profile when feedback is received
 *
 * This function is one of the functions that must exist in one profile for
 * the framework to work.
 *
 * @param context            The compression context
 * @param feedback_type      The feedback type among ROHC_FEEDBACK_1 and ROHC_FEEDBACK_2
 * @param packet             The whole feedback packet with CID bits
 * @param packet_len         The length of the whole feedback packet with CID bits
 * @param feedback_data      The feedback data without the CID bits
 * @param feedback_data_len  The length of the feedback data without the CID bits
 * @return                   true if the feedback was successfully handled,
 *                           false if the feedback could not be taken into account
 */
static bool rohc_comp_rfc5225_ip_feedback(struct rohc_comp_ctxt *const context,
                                          const enum rohc_feedback_type feedback_type __attribute__((unused)),
                                          const uint8_t *const packet __attribute__((unused)),
                                          const size_t packet_len __attribute__((unused)),
                                          const uint8_t *const feedback_data __attribute__((unused)),
                                          const size_t feedback_data_len __attribute__((unused)))
{
	/* TODO: ignore feedback for the moment */
	rohc_comp_debug(context, "TODO: ignore feedback for the moment");
	return true;
}


/**
 * @brief Decide the state that should be used for the next packet
 *
 * @param context  The compression context
 * @param pkt_time The time of packet arrival
 */
static void rohc_comp_rfc5225_ip_decide_state(struct rohc_comp_ctxt *const context,
                                              const struct rohc_ts pkt_time)
{
	const rohc_comp_state_t curr_state = context->state;
	rohc_comp_state_t next_state;

	if(curr_state == ROHC_COMP_STATE_IR)
	{
		if(context->ir_count < MAX_IR_COUNT)
		{
			rohc_comp_debug(context, "not enough packets transmitted in IR state "
			                "for the moment (%zu/%d), so stay in IR state",
			                context->ir_count, MAX_IR_COUNT);
			next_state = ROHC_COMP_STATE_IR;
		}
		else
		{
			rohc_comp_debug(context, "enough packets transmitted in IR state (%zu/%u), "
			                "go to SO state", context->ir_count, MAX_IR_COUNT);
			next_state = ROHC_COMP_STATE_SO;
		}
	}
	else if(curr_state == ROHC_COMP_STATE_FO)
	{
		if(context->fo_count < MAX_FO_COUNT)
		{
			rohc_comp_debug(context, "not enough packets transmitted in FO state "
			                "for the moment (%zu/%u), so stay in FO state",
			                context->fo_count, MAX_FO_COUNT);
			next_state = ROHC_COMP_STATE_FO;
		}
		else
		{
			rohc_comp_debug(context, "enough packets transmitted in FO state (%zu/%u), "
			                "go to SO state", context->fo_count, MAX_FO_COUNT);
			next_state = ROHC_COMP_STATE_SO;
		}
	}
	else if(curr_state == ROHC_COMP_STATE_SO)
	{
		/* do not change state */
		rohc_comp_debug(context, "stay in SO state");
		next_state = ROHC_COMP_STATE_SO;
		/* TODO: handle NACK and STATIC-NACK */
	}
	else
	{
		rohc_comp_warn(context, "unexpected compressor state %d", curr_state);
		assert(0);
		return;
	}

	rohc_comp_change_state(context, next_state);

	/* periodic refreshes in U-mode only */
	if(context->mode == ROHC_U_MODE)
	{
		rohc_comp_periodic_down_transition(context, pkt_time);
	}
}


/**
 * @brief Decide which packet to send when in the different states
 *
 * @param context           The compression context
 * @return                  \li The packet type among ROHC_PACKET_IR,
 *                              ROHC_PACKET_PT_0_CRC3,
 *                              ROHC_PACKET_NORTP_PT_0_CRC7,
 *                              ROHC_PACKET_NORTP_PT_1_SEQ_ID, or
 *                              ROHC_PACKET_NORTP_PT_2_SEQ_ID
 *                              in case of success
 *                          \li ROHC_PACKET_UNKNOWN in case of failure
 */
static rohc_packet_t rohc_comp_rfc5225_ip_decide_pkt(struct rohc_comp_ctxt *const context)
{
	rohc_packet_t packet_type;

	switch(context->state)
	{
		case ROHC_COMP_STATE_IR: /* The Initialization and Refresh (IR) state */
			rohc_comp_debug(context, "code IR packet");
			packet_type = ROHC_PACKET_IR;
			context->ir_count++;
			break;
		case ROHC_COMP_STATE_FO:
			packet_type = rohc_comp_rfc5225_ip_decide_FO_pkt(context);
			context->fo_count++;
			break;
		case ROHC_COMP_STATE_SO:
			packet_type = rohc_comp_rfc5225_ip_decide_SO_pkt(context);
			context->so_count++;
			break;
		case ROHC_COMP_STATE_UNKNOWN:
		default:
#if defined(NDEBUG) || defined(__KERNEL__) || defined(ENABLE_DEAD_CODE)
			packet_type = ROHC_PACKET_UNKNOWN;
#endif
			assert(0); /* should not happen */
			break;
	}

	return packet_type;
}


/**
 * @brief Decide which packet to send when in FO state
 *
 * @param ctxt  The compression context
 * @return      \li The packet type among ROHC_PACKET_IR,
 *                  ROHC_PACKET_NORTP_PT_0_CRC7, or
 *                  ROHC_PACKET_NORTP_PT_2_SEQ_ID
 *                  in case of success
 *              \li ROHC_PACKET_UNKNOWN in case of failure
 */
static rohc_packet_t rohc_comp_rfc5225_ip_decide_FO_pkt(const struct rohc_comp_ctxt *const ctxt)
{
	const bool crc7_at_least = true;
	const rohc_packet_t packet_type =
		rohc_comp_rfc5225_ip_decide_FO_SO_pkt(ctxt, crc7_at_least);

	assert(packet_type != ROHC_PACKET_PT_0_CRC3);
	assert(packet_type != ROHC_PACKET_NORTP_PT_1_SEQ_ID);

	return packet_type;
}


/**
 * @brief Decide which packet to send when in SO state
 *
 * @param ctxt  The compression context
 * @return      \li The packet type among ROHC_PACKET_IR,
 *                  ROHC_PACKET_PT_0_CRC3,
 *                  ROHC_PACKET_NORTP_PT_0_CRC7,
 *                  ROHC_PACKET_NORTP_PT_1_SEQ_ID, or
 *                  ROHC_PACKET_NORTP_PT_2_SEQ_ID
 *                  in case of success
 *              \li ROHC_PACKET_UNKNOWN in case of failure
 */
static rohc_packet_t rohc_comp_rfc5225_ip_decide_SO_pkt(const struct rohc_comp_ctxt *const ctxt)
{
	const bool crc7_at_least = false;
	return rohc_comp_rfc5225_ip_decide_FO_SO_pkt(ctxt, crc7_at_least);
}


/**
 * @brief Decide which packet to send when in FO or SO state
 *
 * @param ctxt           The compression context
 * @param crc7_at_least  Whether packet types with CRC strictly smaller
 *                       than 7 bits are allowed or not
 * @return               \li The packet type among ROHC_PACKET_IR,
 *                           ROHC_PACKET_PT_0_CRC3,
 *                           ROHC_PACKET_NORTP_PT_0_CRC7,
 *                           ROHC_PACKET_NORTP_PT_1_SEQ_ID, or
 *                           ROHC_PACKET_NORTP_PT_2_SEQ_ID
 *                           in case of success
 *                       \li ROHC_PACKET_UNKNOWN in case of failure
 */
static rohc_packet_t rohc_comp_rfc5225_ip_decide_FO_SO_pkt(const struct rohc_comp_ctxt *const ctxt,
                                                           const bool crc7_at_least)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;
	const rohc_reordering_offset_t reorder_ratio = ctxt->compressor->reorder_ratio;
	const ip_context_t *const innermost_ip_ctxt =
		&(rfc5225_ctxt->ip_contexts[rfc5225_ctxt->ip_contexts_nr - 1]);
	const uint16_t innermost_ip_id = rfc5225_ctxt->tmp.innermost_ip_id;
	const rohc_ip_id_behavior_t innermost_ip_id_behavior =
		innermost_ip_ctxt->ctxt.vx.ip_id_behavior;
	rohc_packet_t packet_type;

	/* use pt_0_crc3 only if:
	 *  - CRC-3 is enough to protect the compression
	 *  - 4 MSN bits are enough
	 *  - the innermost IP-ID is either:
	 *     - random (transmitted in irregular chain),
	 *     - zero (not transmitted at all),
	 *     - sequential and inferred from MSN (and not transmitted at all).
	 *  - the TOS/TC fields of all IP headers shall not be changing
	 *  - the behavior of the innermost IP-ID shall not be changing
	 */
	if(!crc7_at_least &&
	   rohc_comp_rfc5225_is_msn_lsb_possible(&rfc5225_ctxt->msn_wlsb,
	                                         rfc5225_ctxt->msn, reorder_ratio, 4) &&
	   (!rohc_comp_rfc5225_is_ipid_sequential(innermost_ip_id_behavior) ||
	    rohc_comp_rfc5225_is_seq_ipid_inferred(innermost_ip_ctxt, innermost_ip_id)) &&
	   !rfc5225_ctxt->tmp.outer_ip_flag &&
	   !rfc5225_ctxt->tmp.innermost_ip_flag &&
	   !rfc5225_ctxt->tmp.df_changed &&
	   !rfc5225_ctxt->tmp.ip_id_behavior_changed)
	{
		rohc_comp_debug(ctxt, "code pt_0_crc3 packet");
		packet_type = ROHC_PACKET_PT_0_CRC3;
	}
	/* use pt_0_crc7 only if:
	 *  - 6 MSN bits are enough
	 *  - the innermost IP-ID is either:
	 *     - random (transmitted in irregular chain),
	 *     - zero (not transmitted at all),
	 *     - sequential and inferred from MSN (and not transmitted at all).
	 *  - the TOS/TC fields of all IP headers shall not be changing
	 *  - the behavior of the innermost IP-ID shall not be changing
	 */
	else if(rohc_comp_rfc5225_is_msn_lsb_possible(&rfc5225_ctxt->msn_wlsb,
	                                              rfc5225_ctxt->msn,
	                                              reorder_ratio, 6) &&
	        (!rohc_comp_rfc5225_is_ipid_sequential(innermost_ip_id_behavior) ||
	         rohc_comp_rfc5225_is_seq_ipid_inferred(innermost_ip_ctxt,
	                                                innermost_ip_id)) &&
	        !rfc5225_ctxt->tmp.outer_ip_flag &&
	        !rfc5225_ctxt->tmp.innermost_ip_flag &&
	        !rfc5225_ctxt->tmp.df_changed &&
	        !rfc5225_ctxt->tmp.ip_id_behavior_changed)
	{
		rohc_comp_debug(ctxt, "code pt_0_crc7 packet");
		packet_type = ROHC_PACKET_NORTP_PT_0_CRC7;
	}
	/* use pt_1_seq_id only if:
	 *  - CRC-3 is enough to protect the compression
	 *  - 6 MSN bits are enough
	 *  - innermost IP-ID is sequential (swapped or not)
	 *  - 4 innermost IP-ID / SN offset bits are enough
	 *  - the TOS/TC fields of all IP headers shall not be changing
	 *  - the behavior of the innermost IP-ID shall not be changing
	 */
	else if(!crc7_at_least &&
	        rohc_comp_rfc5225_is_msn_lsb_possible(&rfc5225_ctxt->msn_wlsb,
	                                              rfc5225_ctxt->msn,
	                                              reorder_ratio, 6) &&
	        rohc_comp_rfc5225_is_ipid_sequential(innermost_ip_id_behavior) &&
	        wlsb_is_kp_possible_16bits(&rfc5225_ctxt->innermost_ip_id_offset_wlsb,
	                                   rfc5225_ctxt->tmp.innermost_ip_id_offset, 4,
	                                   rohc_interval_get_rfc5225_id_id_p(4)) &&
	        !rfc5225_ctxt->tmp.outer_ip_flag &&
	        !rfc5225_ctxt->tmp.innermost_ip_flag &&
	        !rfc5225_ctxt->tmp.df_changed &&
	        !rfc5225_ctxt->tmp.ip_id_behavior_changed)
	{
		assert(innermost_ip_ctxt->ctxt.vx.version == IPV4);
		rohc_comp_debug(ctxt, "code pt_1_seq_id packet");
		packet_type = ROHC_PACKET_NORTP_PT_1_SEQ_ID;
	}
	/* use pt_2_seq_id only if:
	 *  - innermost IP-ID is sequential (swapped or not)
	 *  - 6 innermost IP-ID / SN offset bits are enough
	 *  - 8 MSN bits are enough
	 *  - the TOS/TC fields of all IP headers shall not be changing
	 *  - the behavior of the innermost IP-ID shall not be changing
	 */
	else if(rohc_comp_rfc5225_is_ipid_sequential(innermost_ip_id_behavior) &&
	        wlsb_is_kp_possible_16bits(&rfc5225_ctxt->innermost_ip_id_offset_wlsb,
	                                   rfc5225_ctxt->tmp.innermost_ip_id_offset, 6,
	                                   rohc_interval_get_rfc5225_id_id_p(6)) &&
	        rohc_comp_rfc5225_is_msn_lsb_possible(&rfc5225_ctxt->msn_wlsb,
	                                              rfc5225_ctxt->msn,
	                                              reorder_ratio, 8) &&
	        !rfc5225_ctxt->tmp.outer_ip_flag &&
	        !rfc5225_ctxt->tmp.innermost_ip_flag &&
	        !rfc5225_ctxt->tmp.df_changed &&
	        !rfc5225_ctxt->tmp.ip_id_behavior_changed)
	{
		rohc_comp_debug(ctxt, "code pt_2_seq_id packet");
		packet_type = ROHC_PACKET_NORTP_PT_2_SEQ_ID;
	}
	else /* fallback on IR packet */
	{
		rohc_comp_debug(ctxt, "code IR packet");
		packet_type = ROHC_PACKET_IR;
	}

	return packet_type;
}


/**
 * @brief Define according to computed shift parameter if msn_lsb() is possible
 *
 * @param wlsb           The W-LSB object
 * @param value          The value to encode using the LSB algorithm
 * @param reorder_ratio  The reordering ratio
 * @param k              The number of bits for encoding
 * @return               true if msn_lsb is possible or not
 */
static bool rohc_comp_rfc5225_is_msn_lsb_possible(const struct c_wlsb *const wlsb,
		                                            const uint16_t value,
                                                  const rohc_reordering_offset_t reorder_ratio,
                                                  const size_t k)
{
	/* compute p according to reorder ratio  */
	rohc_lsb_shift_t p_computed = rohc_interval_get_rfc5225_msn_p(k, reorder_ratio);

	return wlsb_is_kp_possible_16bits(wlsb, value, k, p_computed);
}


/**
 * @brief Whether the given IP-ID is sequential (swapped or not)
 *
 * @param behavior  The IP-ID behavior
 * @return          true if the given IP-ID behavior is sequential or sequential
 *                  swapped, false otherwise
 */
static bool rohc_comp_rfc5225_is_ipid_sequential(const rohc_ip_id_behavior_t behavior)
{
	return (behavior == ROHC_IP_ID_BEHAVIOR_SEQ ||
	        behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP);
}


/**
 * @brief Whether the given IP-ID is inferred from SN
 *
 * The given IP-ID is inferred from SN if:
 *  - the IP header is IPv4,
 *  - the IP-ID behavior is sequential or sequential swapped,
 *  - the new IP-ID value increases from the last IP-ID by the same delta as the MSN.
 *
 * For the IP-only profile, the MSN is generated by the compressor, so the MSN
 * delta is always 1.
 *
 * @param ip_ctxt    The context for the given IP header
 * @param new_ip_id  The new value of the IP-ID
 * @return           true if the given IP-ID is sequential and inferred from MSN,
 *                   false otherwise
 */
static bool rohc_comp_rfc5225_is_seq_ipid_inferred(const ip_context_t *const ip_ctxt,
                                                   const uint16_t new_ip_id)
{
	bool is_inferred;

	if(ip_ctxt->ctxt.vx.version != IPV4)
	{
		is_inferred = false;
	}
	else if(ip_ctxt->ctxt.vx.ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ)
	{
		is_inferred = (new_ip_id == (ip_ctxt->ctxt.v4.last_ip_id + 1));
	}
	else if(ip_ctxt->ctxt.vx.ip_id_behavior == ROHC_IP_ID_BEHAVIOR_SEQ_SWAP)
	{
		is_inferred = (swab16(new_ip_id) == (swab16(ip_ctxt->ctxt.v4.last_ip_id) + 1));
	}
	else
	{
		is_inferred = false;
	}

	return is_inferred;
}


/**
 * @brief Encode an IP packet as IR packet
 *
 * @param context           The compression context
 * @param ip                The outer IP header
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_code_IR_pkt(const struct rohc_comp_ctxt *context,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len)
{
	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;
	size_t first_position;
	size_t crc_position;
	size_t rohc_hdr_len = 0;
	int ret;

	/* parts 1 and 3:
	 *  - part 2 will be placed at 'first_position'
	 *  - part 4 will start at 'counter'
	 */
	ret = code_cid_values(context->compressor->medium.cid_type,
	                      context->cid, rohc_remain_data, rohc_remain_len,
	                      &first_position);
	if(ret < 1)
	{
		rohc_comp_warn(context, "failed to encode %s CID %zu: maybe the %zu-byte "
		               "ROHC buffer is too small",
		               context->compressor->medium.cid_type == ROHC_SMALL_CID ?
		               "small" : "large", context->cid, rohc_remain_len);
		goto error;
	}
	rohc_remain_data += ret;
	rohc_remain_len -= ret;
	rohc_hdr_len += ret;
	rohc_comp_debug(context, "%s CID %zu encoded on %d byte(s)",
	                context->compressor->medium.cid_type == ROHC_SMALL_CID ?
	                "small" : "large", context->cid, ret - 1);

	/* type of packet */
	rohc_pkt[first_position] = ROHC_PACKET_TYPE_IR;
	rohc_comp_debug(context, "packet type = 0x%02x", rohc_pkt[first_position]);

	/* enough room for profile ID and CRC? */
	if(rohc_remain_len < 2)
	{
		rohc_comp_warn(context, "ROHC buffer too small for IR packet: "
		               "2 bytes required for profile ID and CRC, but only "
		               "%zu bytes available", rohc_remain_len);
		goto error;
	}

	/* profile ID */
	rohc_comp_debug(context, "profile ID = 0x%02x", context->profile->id & 0xff);
	rohc_remain_data[0] = context->profile->id & 0xff;
	rohc_remain_data++;
	rohc_remain_len--;
	rohc_hdr_len++;

	/* the CRC is computed later since it must be computed over the whole packet
	 * with an empty CRC field */
	rohc_comp_debug(context, "CRC = 0x00 for CRC calculation");
	crc_position = rohc_hdr_len;
	rohc_remain_data[0] = 0;
	rohc_remain_data++;
	rohc_remain_len--;
	rohc_hdr_len++;

	/* add static chain */
	ret = rohc_comp_rfc5225_ip_static_chain(context, ip, rohc_remain_data,
	                                        rohc_remain_len);
	if(ret < 0)
	{
		rohc_comp_warn(context, "failed to build the static chain of the IR packet");
		goto error;
	}
	rohc_remain_data += ret;
	rohc_remain_len -= ret;
	rohc_hdr_len += ret;
	rohc_comp_dump_buf(context, "current ROHC packet (with static part)",
	                   rohc_pkt, rohc_hdr_len);

	/* add dynamic chain */
	ret = rohc_comp_rfc5225_ip_dyn_chain(context, ip, rohc_remain_data,
	                                     rohc_remain_len);
	if(ret < 0)
	{
		rohc_comp_warn(context, "failed to build the dynamic chain of the IR packet");
		goto error;
	}
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	rohc_remain_data += ret;
	rohc_remain_len -= ret;
#endif
	rohc_hdr_len += ret;
	rohc_comp_dump_buf(context, "current ROHC packet (with dynamic part)",
	                   rohc_pkt, rohc_hdr_len);

	/* IR header was successfully built, compute the CRC */
	rohc_pkt[crc_position] = crc_calculate(ROHC_CRC_TYPE_8, rohc_pkt,
	                                       rohc_hdr_len, CRC_INIT_8,
	                                       context->compressor->crc_table_8);
	rohc_comp_debug(context, "CRC (header length = %zu, crc = 0x%x)",
	                rohc_hdr_len, rohc_pkt[crc_position]);

	rohc_comp_debug(context, "IR packet, length %zu", rohc_hdr_len);
	rohc_comp_dump_buf(context, "current ROHC packet", rohc_pkt, rohc_hdr_len);

	return rohc_hdr_len;

error:
	return -1;
}


/**
 * @brief Encode an IP packet as CO packet
 *
 * @param context           The compression context
 * @param ip                The outer IP header
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @param packet_type       The type of ROHC packet to create
 * @param payload_offset    The offset for the payload in the IP packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_code_CO_pkt(const struct rohc_comp_ctxt *const context,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len,
                                            const rohc_packet_t packet_type,
                                            const size_t payload_offset)
{
	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;
	uint8_t crc_computed;
	uint8_t save_first_byte;
	size_t pos_1st_byte;
	size_t pos_2nd_byte;
	int ret;

	/* let's compute the CRC on uncompressed headers */
	if(packet_type == ROHC_PACKET_PT_0_CRC3 ||
	   packet_type == ROHC_PACKET_NORTP_PT_1_SEQ_ID)
	{
		crc_computed =
			crc_calculate(ROHC_CRC_TYPE_3, ip->data, payload_offset,
			              CRC_INIT_3, context->compressor->crc_table_3);
		rohc_comp_debug(context, "CRC-3 on %zu-byte uncompressed header = 0x%x",
		                payload_offset, crc_computed);
	}
	else
	{
		crc_computed =
			crc_calculate(ROHC_CRC_TYPE_7, ip->data, payload_offset,
			              CRC_INIT_7, context->compressor->crc_table_7);
		rohc_comp_debug(context, "CRC-7 on %zu-byte uncompressed header = 0x%x",
		                payload_offset, crc_computed);
	}

	/* write Add-CID or large CID bytes: 'pos_1st_byte' indicates the location
	 * where first header byte shall be written, 'pos_2nd_byte' indicates the
	 * location where the next header bytes shall be written */
	ret = code_cid_values(context->compressor->medium.cid_type, context->cid,
	                      rohc_remain_data, rohc_remain_len, &pos_1st_byte);
	if(ret < 1)
	{
		rohc_comp_warn(context, "failed to encode %s CID %zu: maybe the "
		               "%zu-byte ROHC buffer is too small",
		               context->compressor->medium.cid_type == ROHC_SMALL_CID ?
		               "small" : "large", context->cid, rohc_remain_len);
		goto error;
	}
	pos_2nd_byte = ret;
	rohc_remain_data += ret;
	rohc_remain_len -= ret;
	rohc_comp_debug(context, "%s CID %zu encoded on %d byte(s)",
	                context->compressor->medium.cid_type == ROHC_SMALL_CID ?
	                "small" : "large", context->cid, ret - 1);

	/* The CO headers are written as a contiguous block. There is a problem in
	 * case of large CIDs. In such a case, the CID octets are not located at the
	 * beginning of the ROHC header. The first CO octet is located before the
	 * CID octet(s) and the remaining CO octets are located after the CID octet(s).
	 * To workaround that situation, the last CID octet is saved before writing
	 * the CO header and restored afterwards */
	save_first_byte = rohc_remain_data[-1];
	rohc_remain_data--;
	rohc_remain_len++;

	/* build the specifi CO header */
	if(packet_type == ROHC_PACKET_PT_0_CRC3)
	{
		/* build the pt_0_crc3 ROHC header */
		ret = rohc_comp_rfc5225_ip_build_pt_0_crc3_pkt(context, crc_computed,
		                                               rohc_remain_data,
		                                               rohc_remain_len);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build pt_0_crc3 packet");
			goto error;
		}
		rohc_remain_data += ret;
		rohc_remain_len -= ret;
	}
	else if(packet_type == ROHC_PACKET_NORTP_PT_0_CRC7)
	{
		/* build the pt_0_crc7 ROHC header */
		ret = rohc_comp_rfc5225_ip_build_pt_0_crc7_pkt(context, crc_computed,
		                                               rohc_remain_data,
		                                               rohc_remain_len);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build pt_0_crc7 packet");
			goto error;
		}
		rohc_remain_data += ret;
		rohc_remain_len -= ret;
	}
	else if(packet_type == ROHC_PACKET_NORTP_PT_1_SEQ_ID)
	{
		/* build the pt_1_seq_id ROHC header */
		ret = rohc_comp_rfc5225_ip_build_pt_1_seq_id_pkt(context, crc_computed,
		                                                 rohc_remain_data,
		                                                 rohc_remain_len);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build pt_1_seq_id packet");
			goto error;
		}
		rohc_remain_data += ret;
		rohc_remain_len -= ret;
	}
	else if(packet_type == ROHC_PACKET_NORTP_PT_2_SEQ_ID)
	{
		/* build the pt_2_seq_id ROHC header */
		ret = rohc_comp_rfc5225_ip_build_pt_2_seq_id_pkt(context, crc_computed,
		                                                 rohc_remain_data,
		                                                 rohc_remain_len);
		if(ret < 0)
		{
			rohc_comp_warn(context, "failed to build pt_2_seq_id packet");
			goto error;
		}
		rohc_remain_data += ret;
		rohc_remain_len -= ret;
	}
	else if(packet_type == ROHC_PACKET_UNKNOWN)
	{
		rohc_comp_warn(context, "failed to find the packet type to encode");
		goto error;
	}
	else
	{
		rohc_comp_warn(context, "packet type %d '%s' not supported by profile",
		               packet_type, rohc_get_packet_descr(packet_type));
		assert(0);
		goto error;
	}

	/* add the irregular chain at the very end of the CO header */
	ret = rohc_comp_rfc5225_ip_irreg_chain(context, ip, rohc_remain_data,
	                                       rohc_remain_len);
	if(ret < 0)
	{
		rohc_comp_warn(context, "failed to build the irregular chain of the CO packet");
		goto error;
	}
#ifndef __clang_analyzer__ /* silent warning about dead in/decrement */
	rohc_remain_data += ret;
#endif
	rohc_remain_len -= ret;

	/* end of workaround: restore the saved octet */
	if(context->compressor->medium.cid_type != ROHC_SMALL_CID)
	{
		rohc_pkt[pos_1st_byte] = rohc_pkt[pos_2nd_byte - 1];
		rohc_pkt[pos_2nd_byte - 1] = save_first_byte;
	}

	rohc_comp_dump_buf(context, "CO packet", rohc_pkt,
	                   rohc_pkt_max_len - rohc_remain_len);

	return (rohc_pkt_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Code the static chain of an ROHCv2 IP-only IR packet
 *
 * @param ctxt              The compression context
 * @param ip                The outer IP header
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_static_chain(const struct rohc_comp_ctxt *const ctxt,
                                             const struct ip_packet *const ip,
                                             uint8_t *const rohc_pkt,
                                             const size_t rohc_pkt_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;

	const uint8_t *remain_data = ip->data;
	size_t remain_len = ip->size;

	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;

	size_t ip_hdr_pos;
	int ret;

	/* add IP parts of static chain */
	for(ip_hdr_pos = 0; ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr; ip_hdr_pos++)
	{
		const struct ip_hdr *const ip_hdr = (struct ip_hdr *) remain_data;
		const bool is_innermost = !!(ip_hdr_pos + 1 == rfc5225_ctxt->ip_contexts_nr);

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(ctxt, "found IPv%d", ip_hdr->version);

		if(ip_hdr->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv4_hdr));

			ret = rohc_comp_rfc5225_ip_static_ipv4_part(ctxt, ipv4, is_innermost,
			                                            rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv4 base header part "
				               "of the static chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip_hdr->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv6_hdr));

			ret = rohc_comp_rfc5225_ip_static_ipv6_part(ctxt, ipv6, is_innermost,
			                                            rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv6 base header part "
				               "of the static chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* TODO: handle IPv6 extension headers */
		}
		else
		{
			rohc_comp_warn(ctxt, "unexpected IP version %u", ip_hdr->version);
			assert(0);
			goto error;
		}
	}

	return (rohc_pkt_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the static part of the IPv4 header
 *
 * @param ctxt            The compression context
 * @param ipv4            The IPv4 header
 * @param is_innermost    Whether the IPv4 header is the innermost IP header
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_static_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                                 const struct ipv4_hdr *const ipv4,
                                                 const bool is_innermost,
                                                 uint8_t *const rohc_data,
                                                 const size_t rohc_max_len)
{
	ipv4_static_t *const ipv4_static = (ipv4_static_t *) rohc_data;
	const size_t ipv4_static_len = sizeof(ipv4_static_t);

	if(rohc_max_len < ipv4_static_len)
	{
		rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 static part: "
		               "%zu bytes required, but only %zu bytes available",
		               ipv4_static_len, rohc_max_len);
		goto error;
	}

	ipv4_static->version_flag = 0;
	ipv4_static->innermost_ip = GET_REAL(is_innermost);
	ipv4_static->reserved = 0;
	ipv4_static->protocol = ipv4->protocol;
	rohc_comp_debug(ctxt, "IPv4 protocol = %u", ipv4_static->protocol);
	ipv4_static->src_addr = ipv4->saddr;
	ipv4_static->dst_addr = ipv4->daddr;

	rohc_comp_dump_buf(ctxt, "IPv4 static part", rohc_data, ipv4_static_len);

	return ipv4_static_len;

error:
	return -1;
}


/**
 * @brief Build the static part of the IPv6 header
 *
 * @param ctxt            The compression context
 * @param ipv6            The IPv6 header
 * @param is_innermost    Whether the IPv6 header is the innermost IP header
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_static_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                                 const struct ipv6_hdr *const ipv6,
                                                 const bool is_innermost,
                                                 uint8_t *const rohc_data,
                                                 const size_t rohc_max_len)
{
	size_t ipv6_static_len;

	if(ipv6->flow1 == 0 && ipv6->flow2 == 0)
	{
		ipv6_static_nofl_t *const ipv6_static = (ipv6_static_nofl_t *) rohc_data;

		ipv6_static_len = sizeof(ipv6_static_nofl_t);
		if(rohc_max_len < ipv6_static_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv6 static part: "
			               "%zu bytes required, but only %zu bytes available",
			               ipv6_static_len, rohc_max_len);
			goto error;
		}

		ipv6_static->version_flag = 1;
		ipv6_static->innermost_ip = GET_REAL(is_innermost);
		ipv6_static->reserved1 = 0;
		ipv6_static->flow_label_enc_discriminator = 0;
		ipv6_static->reserved2 = 0;
		ipv6_static->next_header = ipv6->nh;
		memcpy(ipv6_static->src_addr, &ipv6->saddr, sizeof(struct ipv6_addr));
		memcpy(ipv6_static->dst_addr, &ipv6->daddr, sizeof(struct ipv6_addr));
	}
	else
	{
		ipv6_static_fl_t *const ipv6_static = (ipv6_static_fl_t *) rohc_data;

		ipv6_static_len = sizeof(ipv6_static_fl_t);
		if(rohc_max_len < ipv6_static_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv6 static part: "
			               "%zu bytes required, but only %zu bytes available",
			               ipv6_static_len, rohc_max_len);
			goto error;
		}

		ipv6_static->version_flag = 1;
		ipv6_static->innermost_ip = GET_REAL(is_innermost);
		ipv6_static->reserved = 0;
		ipv6_static->flow_label_enc_discriminator = 1;
		ipv6_static->flow_label_msb = ipv6->flow1;
		ipv6_static->flow_label_lsb = ipv6->flow2;
		ipv6_static->next_header = ipv6->nh;
		memcpy(ipv6_static->src_addr, &ipv6->saddr, sizeof(struct ipv6_addr));
		memcpy(ipv6_static->dst_addr, &ipv6->daddr, sizeof(struct ipv6_addr));
	}
	rohc_comp_debug(ctxt, "IPv6 next header = %u", ipv6->nh);

	rohc_comp_dump_buf(ctxt, "IPv6 static part", rohc_data, ipv6_static_len);

	return ipv6_static_len;

error:
	return -1;
}


/**
 * @brief Code the dynamic chain of a ROHCv2 IP-only IR packet
 *
 * @param ctxt              The compression context
 * @param ip                The outer IP header
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_dyn_chain(const struct rohc_comp_ctxt *const ctxt,
                                          const struct ip_packet *const ip,
                                          uint8_t *const rohc_pkt,
                                          const size_t rohc_pkt_max_len)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;

	const uint8_t *remain_data = ip->data;
	size_t remain_len = ip->size;

	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;

	size_t ip_hdr_pos;
	int ret;

	/* there is at least one IP header otherwise it won't be the IP-only profile */
	assert(rfc5225_ctxt->ip_contexts_nr > 0);

	/* add dynamic part for all IP headers */
	for(ip_hdr_pos = 0; ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr; ip_hdr_pos++)
	{
		const struct ip_hdr *const ip_hdr = (struct ip_hdr *) remain_data;
		const ip_context_t *const ip_ctxt = &(rfc5225_ctxt->ip_contexts[ip_hdr_pos]);
		const bool is_innermost = !!(ip_hdr_pos + 1 == rfc5225_ctxt->ip_contexts_nr);

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(ctxt, "found IPv%d", ip_hdr->version);

		if(ip_hdr->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv4_hdr));

			ret = rohc_comp_rfc5225_ip_dyn_ipv4_part(ctxt, ip_ctxt, ipv4, is_innermost,
			                                         rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv4 base header part "
				               "of the dynamic chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip_hdr->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv6_hdr));

			ret = rohc_comp_rfc5225_ip_dyn_ipv6_part(ctxt, ip_ctxt, ipv6, is_innermost,
			                                         rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv6 base header part "
				               "of the dynamic chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* TODO: handle IPv6 extension headers */
		}
		else
		{
			rohc_comp_warn(ctxt, "unexpected IP version %u", ip_hdr->version);
			assert(0);
			goto error;
		}
	}

	return (rohc_pkt_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the dynamic part of the IPv4 header
 *
 * @param ctxt            The compression context
 * @param ip_ctxt         The specific IP compression context
 * @param ipv4            The IPv4 header
 * @param is_innermost    true if the IP header is the innermost of the packet,
 *                        false otherwise
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_dyn_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                              const ip_context_t *const ip_ctxt,
                                              const struct ipv4_hdr *const ipv4,
                                              const bool is_innermost,
                                              uint8_t *const rohc_data,
                                              const size_t rohc_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;
	size_t ipv4_dyn_len;

	assert(ip_ctxt->ctxt.vx.version == IPV4);

	if(is_innermost)
	{
		ipv4_endpoint_innermost_dynamic_noipid_t *const ipv4_dynamic =
			(ipv4_endpoint_innermost_dynamic_noipid_t *) rohc_data;
		ipv4_dyn_len = sizeof(ipv4_endpoint_innermost_dynamic_noipid_t);

		if(rohc_max_len < ipv4_dyn_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 dynamic part: "
			               "%zu bytes required, but only %zu bytes available",
			               ipv4_dyn_len, rohc_max_len);
			goto error;
		}

		ipv4_dynamic->reserved = 0;
		ipv4_dynamic->reorder_ratio = ctxt->compressor->reorder_ratio;
		ipv4_dynamic->df = ipv4->df;
		ipv4_dynamic->ip_id_behavior_innermost = ip_ctxt->ctxt.v4.ip_id_behavior;
		ipv4_dynamic->tos_tc = ipv4->tos;
		ipv4_dynamic->ttl_hopl = ipv4->ttl;

		/* IP-ID */
		if(ipv4_dynamic->ip_id_behavior_innermost == ROHC_IP_ID_BEHAVIOR_ZERO)
		{
			rohc_comp_debug(ctxt, "ip_id_behavior_innermost = %d",
			                ipv4_dynamic->ip_id_behavior_innermost);

			/* MSN */
			ipv4_dynamic->msn = rohc_hton16(rfc5225_ctxt->msn);
		}
		else
		{
			ipv4_endpoint_innermost_dynamic_ipid_t *const ipv4_dynamic_ipid =
				(ipv4_endpoint_innermost_dynamic_ipid_t *) rohc_data;
			ipv4_dyn_len = sizeof(ipv4_endpoint_innermost_dynamic_ipid_t);

			if(rohc_max_len < ipv4_dyn_len)
			{
				rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 dynamic part: "
				               "%zu bytes required, but only %zu bytes available",
				               ipv4_dyn_len, rohc_max_len);
				goto error;
			}

			ipv4_dynamic_ipid->ip_id_innermost = ipv4->id;
			rohc_comp_debug(ctxt, "ip_id_behavior_innermost = %d, IP-ID = 0x%04x",
			                ipv4_dynamic->ip_id_behavior_innermost,
			                rohc_ntoh16(ipv4->id));

			/* MSN */
			ipv4_dynamic_ipid->msn = rohc_hton16(rfc5225_ctxt->msn);
		}
	}
	else /* any outer IPv4 header */
	{
		ipv4_outer_dynamic_noipid_t *const ipv4_dynamic =
			(ipv4_outer_dynamic_noipid_t *) rohc_data;
		ipv4_dyn_len = sizeof(ipv4_outer_dynamic_noipid_t);

		if(rohc_max_len < ipv4_dyn_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 dynamic part: "
			               "%zu bytes required, but only %zu bytes available",
			               ipv4_dyn_len, rohc_max_len);
			goto error;
		}

		ipv4_dynamic->reserved = 0;
		ipv4_dynamic->df = ipv4->df;
		ipv4_dynamic->ip_id_behavior_outer = ip_ctxt->ctxt.v4.ip_id_behavior;
		ipv4_dynamic->tos_tc = ipv4->tos;
		ipv4_dynamic->ttl_hopl = ipv4->ttl;

		/* IP-ID */
		if(ipv4_dynamic->ip_id_behavior_outer == ROHC_IP_ID_BEHAVIOR_ZERO)
		{
			rohc_comp_debug(ctxt, "ip_id_behavior_outer = %d",
			                ipv4_dynamic->ip_id_behavior_outer);
		}
		else
		{
			ipv4_outer_dynamic_ipid_t *const ipv4_dynamic_ipid =
				(ipv4_outer_dynamic_ipid_t *) rohc_data;
			ipv4_dyn_len = sizeof(ipv4_outer_dynamic_ipid_t);

			if(rohc_max_len < ipv4_dyn_len)
			{
				rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 dynamic part: "
				               "%zu bytes required, but only %zu bytes available",
				               ipv4_dyn_len, rohc_max_len);
				goto error;
			}

			ipv4_dynamic_ipid->ip_id_outer = ipv4->id;
			rohc_comp_debug(ctxt, "ip_id_behavior = %d, IP-ID = 0x%04x",
			                ipv4_dynamic->ip_id_behavior_outer, rohc_ntoh16(ipv4->id));
		}
	}

#if 0
	/* TODO: should not update context there */
	ip_ctxt->ctxt.v4.ip_id_behavior = ipv4_dynamic1->ip_id_behavior;
	ip_ctxt->ctxt.v4.last_ip_id_behavior = ip_ctxt->ctxt.v4.ip_id_behavior;
	ip_ctxt->ctxt.v4.dscp = ipv4->dscp;
	ip_ctxt->ctxt.v4.ttl = ipv4->ttl;
	ip_ctxt->ctxt.v4.df = ipv4->df;
	ip_ctxt->ctxt.v4.last_ip_id = rohc_ntoh16(ipv4->id);
#endif

	rohc_comp_dump_buf(ctxt, "IPv4 dynamic part", rohc_data, ipv4_dyn_len);

	return ipv4_dyn_len;

error:
	return -1;
}


/**
 * @brief Build the dynamic part of the IPv6 header
 *
 * @param ctxt            The compression context
 * @param ip_ctxt         The specific IP compression context
 * @param ipv6            The IPv6 header
 * @param is_innermost    true if the IP header is the innermost of the packet,
 *                        false otherwise
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_dyn_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                              const ip_context_t *const ip_ctxt,
                                              const struct ipv6_hdr *const ipv6,
                                              const bool is_innermost,
                                              uint8_t *const rohc_data,
                                              const size_t rohc_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;
	ipv6_regular_dynamic_t *const ipv6_dynamic =
		(ipv6_regular_dynamic_t *) rohc_data;
	size_t ipv6_dyn_len = sizeof(ipv6_regular_dynamic_t);
	const uint8_t tc = ipv6_get_tc(ipv6);

	assert(ip_ctxt->ctxt.v6.version == IPV6);

	if(rohc_max_len < ipv6_dyn_len)
	{
		rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv6 dynamic part: "
		               "%zu bytes required, but only %zu bytes available",
		               ipv6_dyn_len, rohc_max_len);
		goto error;
	}

	ipv6_dynamic->tos_tc = tc;
	ipv6_dynamic->ttl_hopl = ipv6->hl;

	if(is_innermost)
	{
		ipv6_endpoint_dynamic_t *const ipv6_endpoint_dynamic =
			(ipv6_endpoint_dynamic_t *) rohc_data;
		ipv6_dyn_len = sizeof(ipv6_endpoint_dynamic_t);

		if(rohc_max_len < ipv6_dyn_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv6 dynamic part: "
			               "%zu bytes required, but only %zu bytes available",
			               ipv6_dyn_len, rohc_max_len);
			goto error;
		}

		ipv6_endpoint_dynamic->reorder_ratio = ctxt->compressor->reorder_ratio;
		ipv6_endpoint_dynamic->reserved = 0;

		/* MSN */
		ipv6_endpoint_dynamic->msn = rohc_hton16(rfc5225_ctxt->msn);
	}

#if 0
	/* TODO: should not update context there */
	ip_ctxt->ctxt.v6.dscp = dscp;
	ip_ctxt->ctxt.v6.hopl = ipv6->hl;
#endif

	rohc_comp_dump_buf(ctxt, "IP dynamic part", rohc_data, ipv6_dyn_len);

	return ipv6_dyn_len;

error:
	return -1;
}


/**
 * @brief Code the irregular chain of a ROHCv2 IP-only IR packet
 *
 * @param ctxt              The compression context
 * @param ip                The outer IP header
 * @param rohc_pkt          OUT: The ROHC packet
 * @param rohc_pkt_max_len  The maximum length of the ROHC packet
 * @return                  The length of the ROHC packet if successful,
 *                          -1 otherwise
 */
static int rohc_comp_rfc5225_ip_irreg_chain(const struct rohc_comp_ctxt *const ctxt,
                                            const struct ip_packet *const ip,
                                            uint8_t *const rohc_pkt,
                                            const size_t rohc_pkt_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;

	const uint8_t *remain_data = ip->data;
	size_t remain_len = ip->size;

	uint8_t *rohc_remain_data = rohc_pkt;
	size_t rohc_remain_len = rohc_pkt_max_len;

	size_t ip_hdr_pos;
	int ret;

	/* there is at least one IP header otherwise it won't be the IP-only profile */
	assert(rfc5225_ctxt->ip_contexts_nr > 0);

	/* add dynamic part for all IP headers */
	for(ip_hdr_pos = 0; ip_hdr_pos < rfc5225_ctxt->ip_contexts_nr; ip_hdr_pos++)
	{
		const struct ip_hdr *const ip_hdr = (struct ip_hdr *) remain_data;
		const ip_context_t *const ip_ctxt = &(rfc5225_ctxt->ip_contexts[ip_hdr_pos]);
		const bool is_innermost = !!(ip_hdr_pos + 1 == rfc5225_ctxt->ip_contexts_nr);

		/* retrieve IP version */
		assert(remain_len >= sizeof(struct ip_hdr));
		rohc_comp_debug(ctxt, "found IPv%d", ip_hdr->version);

		if(ip_hdr->version == IPV4)
		{
			const struct ipv4_hdr *const ipv4 = (struct ipv4_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv4_hdr));

			ret = rohc_comp_rfc5225_ip_irreg_ipv4_part(ctxt, ip_ctxt, ipv4, is_innermost,
			                                           rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv4 base header part "
				               "of the irregular chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv4_hdr);
			remain_len -= sizeof(struct ipv4_hdr);
		}
		else if(ip_hdr->version == IPV6)
		{
			const struct ipv6_hdr *const ipv6 = (struct ipv6_hdr *) remain_data;

			assert(remain_len >= sizeof(struct ipv6_hdr));

			ret = rohc_comp_rfc5225_ip_irreg_ipv6_part(ctxt, ip_ctxt, ipv6, is_innermost,
			                                           rohc_remain_data, rohc_remain_len);
			if(ret < 0)
			{
				rohc_comp_warn(ctxt, "failed to build the IPv6 base header part "
				               "of the irregular chain");
				goto error;
			}
			rohc_remain_data += ret;
			rohc_remain_len -= ret;

			remain_data += sizeof(struct ipv6_hdr);
			remain_len -= sizeof(struct ipv6_hdr);

			/* TODO: handle IPv6 extension headers */
		}
		else
		{
			rohc_comp_warn(ctxt, "unexpected IP version %u", ip_hdr->version);
			assert(0);
			goto error;
		}
	}

	return (rohc_pkt_max_len - rohc_remain_len);

error:
	return -1;
}


/**
 * @brief Build the irregular part of the IPv4 header
 *
 * @param ctxt            The compression context
 * @param ip_ctxt         The specific IP compression context
 * @param ipv4            The IPv4 header
 * @param is_innermost    true if the IP header is the innermost of the packet,
 *                        false otherwise
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_irreg_ipv4_part(const struct rohc_comp_ctxt *const ctxt,
                                                const ip_context_t *const ip_ctxt,
                                                const struct ipv4_hdr *const ipv4,
                                                const bool is_innermost,
                                                uint8_t *const rohc_data,
                                                const size_t rohc_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;
	uint8_t *rohc_remain_data = rohc_data;
	size_t rohc_remain_len = rohc_max_len;
	size_t ipv4_irreg_len = 0;

	assert(ip_ctxt->ctxt.vx.version == IPV4);

	/* IP ID if random */
	if(ip_ctxt->ctxt.v4.ip_id_behavior == ROHC_IP_ID_BEHAVIOR_RAND)
	{
		if(rohc_remain_len < sizeof(uint16_t))
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 header "
			               "irregular part: %zu bytes required for random IP-ID, "
			               "but only %zu bytes available", sizeof(uint16_t),
			               rohc_remain_len);
			goto error;
		}
		memcpy(rohc_remain_data, &ipv4->id, sizeof(uint16_t));
		rohc_remain_data += sizeof(uint16_t);
		rohc_remain_len -= sizeof(uint16_t);
		ipv4_irreg_len += sizeof(uint16_t);
		rohc_comp_debug(ctxt, "random IP-ID 0x%04x", rohc_ntoh16(ipv4->id));
	}

	/* TOS and TTL for outer IP headers */
	if(!is_innermost && rfc5225_ctxt->tmp.outer_ip_flag)
	{
		const size_t tos_ttl_req_len = 2;

		if(rohc_remain_len < tos_ttl_req_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv4 header "
			               "irregular part: %zu bytes required for TOS and TTL, "
			               "but only %zu bytes available", tos_ttl_req_len,
			               rohc_remain_len);
			goto error;
		}
		rohc_remain_data[0] = ipv4->tos;
		rohc_remain_data[1] = ipv4->ttl;
		rohc_remain_data += tos_ttl_req_len;
		rohc_remain_len -= tos_ttl_req_len;
		ipv4_irreg_len += tos_ttl_req_len;
	}

	rohc_comp_dump_buf(ctxt, "IPv4 irregular part", rohc_data, ipv4_irreg_len);

	return ipv4_irreg_len;

error:
	return -1;
}


/**
 * @brief Build the irregular part of the IPv6 header
 *
 * @param ctxt            The compression context
 * @param ip_ctxt         The specific IP compression context
 * @param ipv6            The IPv6 header
 * @param is_innermost    true if the IP header is the innermost of the packet,
 *                        false otherwise
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_irreg_ipv6_part(const struct rohc_comp_ctxt *const ctxt,
                                                const ip_context_t *const ip_ctxt,
                                                const struct ipv6_hdr *const ipv6,
                                                const bool is_innermost,
                                                uint8_t *const rohc_data,
                                                const size_t rohc_max_len)
{
	const struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = ctxt->specific;
	uint8_t *rohc_remain_data = rohc_data;
	size_t rohc_remain_len = rohc_max_len;
	size_t ipv6_irreg_len = 0;

	assert(ip_ctxt->ctxt.v6.version == IPV6);

	/* TOS and TTL for outer IP headers */
	if(!is_innermost && rfc5225_ctxt->tmp.outer_ip_flag)
	{
		const size_t tc_hl_req_len = 2;

		if(rohc_remain_len < tc_hl_req_len)
		{
			rohc_comp_warn(ctxt, "ROHC buffer too small for the IPv6 header "
			               "irregular part: %zu bytes required for TC and HL, "
			               "but only %zu bytes available", tc_hl_req_len,
			               rohc_remain_len);
			goto error;
		}
		rohc_remain_data[0] = ipv6_get_tc(ipv6);
		rohc_remain_data[1] = ipv6->hl;
		rohc_remain_data += tc_hl_req_len;
		rohc_remain_len -= tc_hl_req_len;
		ipv6_irreg_len += tc_hl_req_len;
	}

	rohc_comp_dump_buf(ctxt, "IPv6 irregular part", rohc_data, ipv6_irreg_len);

	return ipv6_irreg_len;

error:
	return -1;
}


/**
 * @brief Build a ROHCv2 pt_0_crc3 packet
 *
 * @param context         The compression context
 * @param crc             The CRC on the uncompressed headers
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_build_pt_0_crc3_pkt(const struct rohc_comp_ctxt *const context,
                                                    const uint8_t crc,
                                                    uint8_t *const rohc_data,
                                                    const size_t rohc_max_len)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;
	pt_0_crc3_t *const pt_0_crc3 = (pt_0_crc3_t *) rohc_data;

	if(rohc_max_len < sizeof(pt_0_crc3_t))
	{
		rohc_comp_warn(context, "ROHC buffer too small for the pt_0_crc3_t header: "
		               "%zu bytes required, but only %zu bytes available",
		               sizeof(pt_0_crc3_t), rohc_max_len);
		goto error;
	}

	pt_0_crc3->discriminator = 0x0;
	pt_0_crc3->msn = rfc5225_ctxt->msn & 0xf;
	pt_0_crc3->header_crc = crc;

	return sizeof(pt_0_crc3_t);

error:
	return -1;
}


/**
 * @brief Build a ROHCv2 pt_0_crc7 packet
 *
 * @param context         The compression context
 * @param crc             The CRC on the uncompressed headers
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_build_pt_0_crc7_pkt(const struct rohc_comp_ctxt *const context,
                                                    const uint8_t crc,
                                                    uint8_t *const rohc_data,
                                                    const size_t rohc_max_len)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;
	pt_0_crc7_t *const pt_0_crc7 = (pt_0_crc7_t *) rohc_data;

	if(rohc_max_len < sizeof(pt_0_crc7_t))
	{
		rohc_comp_warn(context, "ROHC buffer too small for the pt_0_crc7_t header: "
		               "%zu bytes required, but only %zu bytes available",
		               sizeof(pt_0_crc7_t), rohc_max_len);
		goto error;
	}

	pt_0_crc7->discriminator = 0x4;
	pt_0_crc7->msn_1 = (rfc5225_ctxt->msn >> 1) & 0x1f;
	pt_0_crc7->msn_2 = rfc5225_ctxt->msn & 0x01;
	pt_0_crc7->header_crc = crc;

	return sizeof(pt_0_crc7_t);

error:
	return -1;
}


/**
 * @brief Build a ROHCv2 pt_1_seq_id packet
 *
 * @param context         The compression context
 * @param crc             The CRC on the uncompressed headers
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_build_pt_1_seq_id_pkt(const struct rohc_comp_ctxt *const context,
                                                      const uint8_t crc,
                                                      uint8_t *const rohc_data,
                                                      const size_t rohc_max_len)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;
	pt_1_seq_id_t *const pt_1_seq_id = (pt_1_seq_id_t *) rohc_data;

	if(rohc_max_len < sizeof(pt_1_seq_id_t))
	{
		rohc_comp_warn(context, "ROHC buffer too small for the pt_1_seq_id header: "
		               "%zu bytes required, but only %zu bytes available",
		               sizeof(pt_1_seq_id_t), rohc_max_len);
		goto error;
	}

	pt_1_seq_id->discriminator = 0x5;
	pt_1_seq_id->header_crc = crc;
	pt_1_seq_id->msn_1 = (rfc5225_ctxt->msn >> 4) & 0x03;
	pt_1_seq_id->msn_2 = rfc5225_ctxt->msn & 0x0f;
	pt_1_seq_id->ip_id = rfc5225_ctxt->tmp.innermost_ip_id_offset & 0x0f;

	return sizeof(pt_1_seq_id_t);

error:
	return -1;
}


/**
 * @brief Build a ROHCv2 pt_2_seq_id packet
 *
 * @param context         The compression context
 * @param crc             The CRC on the uncompressed headers
 * @param[out] rohc_data  The ROHC packet being built
 * @param rohc_max_len    The max remaining length in the ROHC buffer
 * @return                The length appended in the ROHC buffer if positive,
 *                        -1 in case of error
 */
static int rohc_comp_rfc5225_ip_build_pt_2_seq_id_pkt(const struct rohc_comp_ctxt *const context,
                                                      const uint8_t crc,
                                                      uint8_t *const rohc_data,
                                                      const size_t rohc_max_len)
{
	struct rohc_comp_rfc5225_ip_ctxt *const rfc5225_ctxt = context->specific;
	pt_2_seq_id_t *const pt_2_seq_id = (pt_2_seq_id_t *) rohc_data;

	if(rohc_max_len < sizeof(pt_2_seq_id_t))
	{
		rohc_comp_warn(context, "ROHC buffer too small for the pt_2_seq_id header: "
		               "%zu bytes required, but only %zu bytes available",
		               sizeof(pt_2_seq_id_t), rohc_max_len);
		goto error;
	}

	pt_2_seq_id->discriminator = 0x6;
	pt_2_seq_id->ip_id_1 = (rfc5225_ctxt->tmp.innermost_ip_id_offset >> 1) & 0x1f;
	pt_2_seq_id->ip_id_2 = rfc5225_ctxt->tmp.innermost_ip_id_offset & 0x01;
	pt_2_seq_id->header_crc = crc;
	pt_2_seq_id->msn = rfc5225_ctxt->msn & 0xff;

	return sizeof(pt_2_seq_id_t);

error:
	return -1;
}


/**
 * @brief Define the compression part of the ROHCv2 IP-only profile as described
 *        in the RFC 5225
 */
const struct rohc_comp_profile rohc_comp_rfc5225_ip_profile =
{
	.id             = ROHCv2_PROFILE_IP, /* profile ID (RFC5225, ROHCv2 IP) */
	.protocol       = 0,                               /* IP protocol */
	.create         = rohc_comp_rfc5225_ip_create,     /* profile handlers */
	.clone          = NULL,
	.destroy        = rohc_comp_rfc5225_ip_destroy,
	.check_profile  = rohc_comp_rfc5225_ip_check_profile,
	.check_context  = rohc_comp_rfc5225_ip_check_context,
	.encode         = rohc_comp_rfc5225_ip_encode,
	.feedback       = rohc_comp_rfc5225_ip_feedback,
};

