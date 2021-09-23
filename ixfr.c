/*
 * ixfr.c -- generating IXFR responses.
 *
 * Copyright (c) 2021, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"

#include "ixfr.h"
#include "packet.h"
#include "rdata.h"

/*
 * For optimal compression IXFR response packets are limited in size
 * to MAX_COMPRESSION_OFFSET.
 */
#define IXFR_MAX_MESSAGE_LEN MAX_COMPRESSION_OFFSET

/* initial space in rrs data for storing records */
#define IXFR_STORE_INITIAL_SIZE 4096

query_state_type query_ixfr(struct nsd *nsd, struct query *query)
{
	(void)nsd;
	(void)query;
	return QUERY_PROCESSED;
}

/* free ixfr_data structure */
static void ixfr_data_free(struct ixfr_data* data)
{
	if(!data)
		return;
	free(data->newsoa);
	free(data->oldsoa);
	free(data->del);
	free(data->add);
	free(data);
}

struct ixfr_store* ixfr_store_start(struct zone* zone,
	struct ixfr_store* ixfr_store_mem, uint32_t old_serial,
	uint32_t new_serial)
{
	struct ixfr_store* ixfr_store = ixfr_store_mem;
	memset(ixfr_store, 0, sizeof(*ixfr_store));
	ixfr_store->zone = zone;
	ixfr_store->data = xalloc_zero(sizeof(*ixfr_store->data));
	ixfr_store->data->oldserial = old_serial;
	ixfr_store->data->newserial = new_serial;
	return ixfr_store;
}

void ixfr_store_cancel(struct ixfr_store* ixfr_store)
{
	ixfr_store->cancelled = 1;
	ixfr_data_free(ixfr_store->data);
	ixfr_store->data = NULL;
}

void ixfr_store_free(struct ixfr_store* ixfr_store)
{
	if(!ixfr_store)
		return;
	ixfr_data_free(ixfr_store->data);
}

/* make space in record data for the new size, grows the allocation */
static void ixfr_make_space(uint8_t** rrs, size_t* len, size_t* capacity,
	size_t added)
{
	size_t newsize = 0;
	if(*rrs == NULL) {
		newsize = IXFR_STORE_INITIAL_SIZE;
	} else {
		if(*len + added <= *capacity)
			return; /* already enough space */
		newsize = (*capacity)*2;
	}
	if(*len + added > newsize)
		newsize = *len + added;
	if(*rrs == NULL) {
		*rrs = xalloc(newsize);
	} else {
		*rrs = xrealloc(*rrs, newsize);
	}
	*capacity = newsize;
}

/* put new SOA record after delrrs and addrrs */
static void ixfr_put_newsoa(struct ixfr_store* ixfr_store, uint8_t** rrs,
	size_t* len, size_t* capacity)
{
	uint8_t* soa = ixfr_store->data->newsoa;
	size_t soa_len = ixfr_store->data->newsoa_len;
	ixfr_make_space(rrs, len, capacity, soa_len);
	if(!*rrs || *len + soa_len > *capacity) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot allocate space");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	memmove(*rrs + *len, soa, soa_len);
	*len += soa_len;
}

/* trim unused storage from the rrs data */
static void ixfr_trim_capacity(uint8_t** rrs, size_t* len, size_t* capacity)
{
	if(*rrs == NULL)
		return;
	if(*capacity == *len)
		return;
	*rrs = xrealloc(*rrs, *len);
	*capacity = *len;
}

void ixfr_store_finish(struct ixfr_store* ixfr_store, char* log_buf,
	uint64_t time_start_0, uint32_t time_start_1, uint64_t time_end_0,
	uint32_t time_end_1)
{
	if(ixfr_store->cancelled) {
		ixfr_store_free(ixfr_store);
		return;
	}

	/* put new serial SOA record after delrrs and addrrs */
	ixfr_put_newsoa(ixfr_store, &ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
	ixfr_put_newsoa(ixfr_store, &ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);

	/* trim the data in the store, the overhead from capacity is
	 * removed */
	ixfr_trim_capacity(&ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
	ixfr_trim_capacity(&ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);

	if(ixfr_store->cancelled) {
		ixfr_store_free(ixfr_store);
		return;
	}

	/* store the data in the zone */
	if(!ixfr_store->zone->ixfr)
		ixfr_store->zone->ixfr = zone_ixfr_create();
	if(ixfr_store->zone->ixfr->data)
		zone_ixfr_remove(ixfr_store->zone->ixfr);
	zone_ixfr_add(ixfr_store->zone->ixfr, ixfr_store->data);
	ixfr_store->data = NULL;

	(void)log_buf;
	(void)time_start_0;
	(void)time_start_1;
	(void)time_end_0;
	(void)time_end_1;

	/* free structure */
	ixfr_store_free(ixfr_store);
}

/* read SOA rdata section for SOA storage */
static int read_soa_rdata(struct buffer* packet, uint8_t* primns,
	int* primns_len, uint8_t* email, int* email_len,
	uint32_t* serial, uint32_t* refresh, uint32_t* retry,
	uint32_t* expire, uint32_t* minimum, size_t* sz)
{
	if(!(*primns_len = dname_make_wire_from_packet(primns, packet, 1))) {
		log_msg(LOG_ERR, "ixfr_store: cannot parse soa nsname in packet");
		return 0;
	}
	*sz += *primns_len;
	if(!(*email_len = dname_make_wire_from_packet(email, packet, 1))) {
		log_msg(LOG_ERR, "ixfr_store: cannot parse soa maintname in packet");
		return 0;
	}
	*sz += *email_len;
	*serial = buffer_read_u32(packet);
	*sz += 4;
	*refresh = buffer_read_u32(packet);
	*sz += 4;
	*retry = buffer_read_u32(packet);
	*sz += 4;
	*expire = buffer_read_u32(packet);
	*sz += 4;
	*minimum = buffer_read_u32(packet);
	*sz += 4;
	return 1;
}

/* store SOA record data in memory buffer */
static void store_soa(uint8_t* soa, struct zone* zone, uint32_t ttl,
	uint16_t rdlen_uncompressed, uint8_t* primns, int primns_len,
	uint8_t* email, int email_len, uint32_t serial, uint32_t refresh,
	uint32_t retry, uint32_t expire, uint32_t minimum)
{
	uint8_t* sp = soa;
	memmove(sp, dname_name(domain_dname(zone->apex)),
		domain_dname(zone->apex)->name_size);
	sp += domain_dname(zone->apex)->name_size;
	write_uint16(sp, TYPE_SOA);
	sp += 2;
	write_uint16(sp, CLASS_IN);
	sp += 2;
	write_uint32(sp, ttl);
	sp += 4;
	write_uint16(sp, rdlen_uncompressed);
	sp += 2;
	memmove(sp, primns, primns_len);
	sp += primns_len;
	memmove(sp, email, email_len);
	sp += email_len;
	write_uint32(sp, serial);
	sp += 4;
	write_uint32(sp, refresh);
	sp += 4;
	write_uint32(sp, retry);
	sp += 4;
	write_uint32(sp, expire);
	sp += 4;
	write_uint32(sp, minimum);
}

void ixfr_store_add_newsoa(struct ixfr_store* ixfr_store,
	struct buffer* packet, size_t ttlpos)
{
	size_t oldpos, sz = 0;
	uint32_t ttl, serial, refresh, retry, expire, minimum;
	uint16_t rdlen_uncompressed, rdlen_wire;
	int primns_len = 0, email_len = 0;
	uint8_t primns[MAXDOMAINLEN + 1], email[MAXDOMAINLEN + 1];

	if(ixfr_store->cancelled)
		return;
	if(ixfr_store->data->newsoa) {
		free(ixfr_store->data->newsoa);
		ixfr_store->data->newsoa = NULL;
		ixfr_store->data->newsoa_len = 0;
	}
	oldpos = buffer_position(packet);
	buffer_set_position(packet, ttlpos);

	/* calculate the length */
	sz = domain_dname(ixfr_store->zone->apex)->name_size;
	sz += 2 /* type */ + 2 /* class */;
	/* read ttl */
	if(!buffer_available(packet, 4/*ttl*/+2/*rdlen*/)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store: not enough space in packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	ttl = buffer_read_u32(packet);
	sz += 4;
	rdlen_wire = buffer_read_u16(packet);
	sz += 2;
	if(!buffer_available(packet, rdlen_wire)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store: not enough rdata space in packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	if(!read_soa_rdata(packet, primns, &primns_len, email, &email_len,
		&serial, &refresh, &retry, &expire, &minimum, &sz)) {
		log_msg(LOG_ERR, "ixfr_store newsoa: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	rdlen_uncompressed = primns_len + email_len + 4 + 4 + 4 + 4 + 4;

	/* store the soa record */
	ixfr_store->data->newsoa = xalloc(sz);
	ixfr_store->data->newsoa_len = sz;
	store_soa(ixfr_store->data->newsoa, ixfr_store->zone, ttl,
		rdlen_uncompressed, primns, primns_len, email, email_len,
		serial, refresh, retry, expire, minimum);

	buffer_set_position(packet, oldpos);
}

void ixfr_store_add_oldsoa(struct ixfr_store* ixfr_store, uint32_t ttl,
	struct buffer* packet, size_t rrlen)
{
	size_t oldpos, sz = 0;
	uint32_t serial, refresh, retry, expire, minimum;
	uint16_t rdlen_uncompressed;
	int primns_len = 0, email_len = 0;
	uint8_t primns[MAXDOMAINLEN + 1], email[MAXDOMAINLEN + 1];

	if(ixfr_store->cancelled)
		return;
	if(ixfr_store->data->oldsoa) {
		free(ixfr_store->data->oldsoa);
		ixfr_store->data->oldsoa = NULL;
		ixfr_store->data->oldsoa_len = 0;
	}
	oldpos = buffer_position(packet);

	/* calculate the length */
	sz = domain_dname(ixfr_store->zone->apex)->name_size;
	sz += 2 /* type */ + 2 /* class */ + 4 /* ttl */;
	if(!buffer_available(packet, rrlen)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store oldsoa: not enough rdata space in packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	if(!read_soa_rdata(packet, primns, &primns_len, email, &email_len,
		&serial, &refresh, &retry, &expire, &minimum, &sz)) {
		log_msg(LOG_ERR, "ixfr_store oldsoa: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	rdlen_uncompressed = primns_len + email_len + 4 + 4 + 4 + 4 + 4;

	/* store the soa record */
	ixfr_store->data->oldsoa = xalloc(sz);
	ixfr_store->data->oldsoa_len = sz;
	store_soa(ixfr_store->data->oldsoa, ixfr_store->zone, ttl,
		rdlen_uncompressed, primns, primns_len, email, email_len,
		serial, refresh, retry, expire, minimum);

	buffer_set_position(packet, oldpos);
}

void ixfr_store_putrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region, uint8_t** rrs,
	size_t* rrs_len, size_t* rrs_capacity)
{
	domain_table_type *temptable;
	rdata_atom_type *rdatas;
	ssize_t rdata_num;
	int i;
	size_t rdlen_uncompressed, sz;
	uint8_t* sp;

	if(ixfr_store->cancelled)
		return;

	/* parse rdata */
	temptable = domain_table_create(temp_region);
	rdata_num = rdata_wireformat_to_rdata_atoms(temp_region, temptable,
		type, rrlen, packet, &rdatas);
	if(rdata_num == -1) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}

	/* find rdatalen */
	rdlen_uncompressed = 0;
	for(i=0; i<rdata_num; i++) {
		if(rdata_atom_is_domain(type, i)) {
			rdlen_uncompressed += domain_dname(rdatas[i].domain)
				->name_size;
		} else {
			rdlen_uncompressed += rdatas[i].data[0];
		}
	}
	sz = dname->name_size + 2 /*type*/ + 2 /*class*/ + 4 /*ttl*/ +
		2 /*rdlen*/ + rdlen_uncompressed;

	/* store RR in IXFR data */
	ixfr_make_space(rrs, rrs_len, rrs_capacity, sz);
	if(!*rrs || *rrs_len + sz > *rrs_capacity) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot allocate space");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	/* copy data into add */
	sp = *rrs + *rrs_len;
	*rrs_len += sz;
	memmove(sp, dname_name(dname), dname->name_size);
	sp += dname->name_size;
	write_uint16(sp, type);
	sp += 2;
	write_uint16(sp, klass);
	sp += 2;
	write_uint32(sp, ttl);
	sp += 4;
	write_uint16(sp, rdlen_uncompressed);
	sp += 2;
	for(i=0; i<rdata_num; i++) {
		if(rdata_atom_is_domain(type, i)) {
			memmove(sp, dname_name(domain_dname(rdatas[i].domain)),
				domain_dname(rdatas[i].domain)->name_size);
			sp += domain_dname(rdatas[i].domain)->name_size;
		} else {
			memmove(sp, &rdatas[i].data[1], rdatas[i].data[0]);
			sp += rdatas[i].data[0];
		}
	}
}

void ixfr_store_delrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region)
{
	ixfr_store_putrr(ixfr_store, dname, type, klass, ttl, packet, rrlen,
		temp_region, &ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
}

void ixfr_store_addrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region)
{
	ixfr_store_putrr(ixfr_store, dname, type, klass, ttl, packet, rrlen,
		temp_region, &ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);
}

int zone_is_ixfr_enabled(struct zone* zone)
{
	(void)zone;
	return 1;
}

struct zone_ixfr* zone_ixfr_create(void)
{
	return xalloc_zero(sizeof(struct zone_ixfr));
}

void zone_ixfr_free(struct zone_ixfr* ixfr)
{
	if(!ixfr)
		return;
	ixfr_data_free(ixfr->data);
}

/* size of the ixfr data */
static size_t ixfr_data_size(struct ixfr_data* data)
{
	return sizeof(struct ixfr_data) + data->newsoa_len + data->oldsoa_len
		+ data->del_len + data->add_len;
}

void zone_ixfr_remove(struct zone_ixfr* ixfr)
{
	ixfr->total_size -= ixfr_data_size(ixfr->data);
	ixfr_data_free(ixfr->data);
	ixfr->data = NULL;
}

void zone_ixfr_add(struct zone_ixfr* ixfr, struct ixfr_data* data)
{
	ixfr->data = data;
	ixfr->total_size += ixfr_data_size(ixfr->data);
}
