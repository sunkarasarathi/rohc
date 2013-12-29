/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file   /comp/schemes/list_ipv6.c
 * @brief  ROHC list compression of IPv6 extension headers
 * @author Didier Barvaux <didier@barvaux.org>
 */

#include "schemes/list_ipv6.h"

#ifndef __KERNEL__
#  include <string.h>
#endif


static int get_index_ipv6_table(const uint8_t next_header_type)
	__attribute__((warn_unused_result, const));

static bool cmp_ipv6_ext(const struct rohc_list_item *const item,
                         const uint8_t ext_type,
                         const uint8_t *const ext_data,
                         const size_t ext_len)
	__attribute__((warn_unused_result, nonnull(1, 3)));


/**
 * @brief Create one context for compressing lists of IPv6 extension headers
 *
 * @param comp            The context to create
 * @param trace_callback  The function to call for printing traces
 * @param profile_id      The ID of the associated decompression profile
 */
void rohc_comp_list_ipv6_new(struct list_comp *const comp,
                             rohc_trace_callback_t trace_callback,
                             const int profile_id)
{
	size_t i;

	memset(comp, 0, sizeof(struct list_comp));

	comp->ref_id = ROHC_LIST_GEN_ID_NONE;
	comp->cur_id = ROHC_LIST_GEN_ID_NONE;

	for(i = 0; i <= ROHC_LIST_GEN_ID_MAX; i++)
	{
		rohc_list_reset(&comp->lists[i]);
		comp->lists[i].id = i;
	}

	rohc_list_reset(&comp->pkt_list);

	for(i = 0; i < ROHC_LIST_MAX_ITEM; i++)
	{
		rohc_list_item_reset(&comp->trans_table[i]);
	}

	/* specific callbacks for IPv6 extension headers */
	comp->get_size = ip_get_extension_size;
	comp->get_index_table = get_index_ipv6_table;
	comp->cmp_item = cmp_ipv6_ext;

	/* traces */
	comp->trace_callback = trace_callback;
	comp->profile_id = profile_id;
}


/**
 * @brief Free one context for compressing lists of IPv6 extension headers
 *
 * @param comp          The context to destroy
 */
void rohc_comp_list_ipv6_free(struct list_comp *const comp)
{
	memset(comp, 0, sizeof(struct list_comp));
}


/**
 * @brief Get the index for the given IPv6 extension type
 *
 * @param next_header_type  The Next Header type to get an index for
 * @return                  The based table index
 */
static int get_index_ipv6_table(const uint8_t next_header_type)
{
	int index_table;

	switch(next_header_type)
	{
		case ROHC_IPPROTO_HOPOPTS:
			index_table = 0;
			break;
		case ROHC_IPPROTO_DSTOPTS:
			index_table = 1;
			break;
		case ROHC_IPPROTO_ROUTING:
			index_table = 2;
			break;
		case ROHC_IPPROTO_AH:
			index_table = 3;
			break;
		default:
			goto error;
	}

	return index_table;

error:
	return -1;
}


/**
 * @brief Compare two IPv6 items
 *
 * @param item      The IPv6 item to compare
 * @param ext_type  The IPv6 Next Header type
 * @param ext_data  The IPv6 extension context
 * @param ext_len   The length (in bytes) of the IPv6 extension context
 * @return          true if the two items are equal,
 *                  false if they are different
 */
static bool cmp_ipv6_ext(const struct rohc_list_item *const item,
                         const uint8_t ext_type,
                         const uint8_t *const ext_data,
                         const size_t ext_len)
{
	/* IPv6 items are equal if:
	 *  - they are of the same type,
	 *  - they got the same length,
	 *  - they are both at least 2-byte length,
	 *  - they got the same content (except for the Next Header byte). */
	return (item->type == ext_type &&
	        item->length == ext_len &&
	        item->length >= 2 &&
	        memcmp(item->data + 1, ext_data + 1, item->length - 1) == 0);
}
