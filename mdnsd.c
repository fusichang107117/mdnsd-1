/*
 * Copyright (C) 2003 Jeremie Miller <jer@jabber.org>
 * Copyright (c) 2009 Simon Budig <simon@budig.org>
 * Copyright (C) 2013 Ole Reinhardt <ole.reinhardt@embedded-it.de>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * For additional information see http://www.ethernut.de/
 */

/* This code is based on
 * Based on BSD licensed mdnsd implementation by Jer <jer@jabber.org>
 * http://dotlocal.org/mdnsd/
 *
 * Unfortunately this site is now longer alive. You can still find it at:
 * http://web.archive.org/web/20080705131510/http://dotlocal.org/mdnsd/
 *
 * mdnsd - embeddable Multicast DNS Daemon
 * =======================================
 *
 * "mdnsd" is a very lightweight, simple, portable, and easy to integrate
 * open source implementation of Multicast DNS (part of Zeroconf, also called
 * Rendezvous by Apple) for developers. It supports both acting as a Query and
 * a Responder, allowing any software to participate fully on the .localnetwork
 * just by including a few files and calling a few functions.  All of the
 * complexity of handling the Multicast DNS retransmit timing, duplicate
 * suppression, probing, conflict detection, and other facets of the DNS
 * protocol is hidden behind a very simple and very easy to use interface,
 * described in the header file. The single small c source file has almost no
 * dependencies, and is portable to almost any embedded platform.
 * Multiple example applications and usages are included in the download,
 * including a simple persistent query browser and a tool to advertise .local
 * web sites.
 *
 * The code is licensed under both the GPL and BSD licenses, for use in any
 * free software or commercial application. If there is a licensing need not
 * covered by either of those, alternative licensing is available upon request.
 *
 */

/*!
 * \file pro/mdnsd.c
 * \brief Multicast DNS Deamon
 *
 * \verbatim
 *
 * $Id$
 *
 * \endverbatim
 */

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <stdio.h>
#include "mdnsd.h"

/*!
 * \addtogroup xgMulticastDns
 */
/*@{*/

#ifdef NUTDEBUG
##define MDNSD_DEBUG
#endif

/*
 * Messy, but it's the best/simplest balance I can find at the moment
 *
 * Some internal data types, and a few hashes:
 * - querys
 * - answers
 * - cached
 * - records (published, unique and shared)
 *
 * Each type has different semantics for processing, both for timeouts,
 * incoming, and outgoing I/O.
 *
 * They inter-relate too, like records affect the querys they are relevant to.
 *
 * Nice things about MDNS: we only publish once (and then ask asked),
 * and only query once, then just expire records we've got cached
 */

/*!
 * \brief Generates a hash code for a string.
 *
 * This function uses the ELF hashing algorithm as reprinted in
 * Andrew Binstock, "Hashing Rehashed," Dr. Dobb's Journal, April 1996.
 *
 * \param s     The string to hash
 *
 * \return      The calculated hash value
 */

static int NameHash(const int8_t *str)
{
    const uint8_t *name = (const uint8_t *)str;
    uint32_t hash = 0;
    uint32_t g;

    while (*name) {
        /* do some fancy bitwanking on the string */
        hash = (hash << 4) + (unsigned long)(tolower (*name++));
        if ((g = (hash & 0xF0000000UL))!=0)
            hash ^= (g >> 24);
        hash &= ~g;
    }

    return (int)hash;
}

/*!
 * \brief Get the next matching query in the hash list
 *
 * Basic hash and linked list primatives for Query hash.
 * Iterate through the given query list and search for the given host.
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param query     The query list head entry to start searching from or NULL
 *                  if a new search shall be started
 * \param host      host name to search for
 * \param type      query type
 *
 * \return          The next matching query or NULL
 */
static TQuery *QueryNext(TMdnsd *mdnsd, TQuery *query, int8_t *host, int type)
{
    if (query == NULL) {
        query = mdnsd->queries[NameHash(host) % SPRIME];
    } else {
        query = query->next;
    }

    for( ; query != NULL; query = query->next) {
        if (((query->type == QTYPE_ANY) || (query->type == type)) &&
            (strcasecmp(query->name, host) == 0)) {
            return query;
        }
    }

    return NULL;
}

/*!
 * \brief Get the next matching cache entry in the hash list
 *
 * Basic hash and linked list primatives for cache hash.
 * Iterate through the given cache list and search for the given host.
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param cached    The cache list head entry to start searching from or NULL
 *                  if a new search shall be started
 * \param host      host name to search for
 * \param type      query type
 *
 * \return          The next matching cache entry or NULL
 */
static TCached *CachedNext(TMdnsd *mdnsd, TCached *cached, uint8_t *host, int type)
{
    if (cached == NULL) {
        cached = mdnsd->cache[NameHash(host) % LPRIME];
    } else {
        cached = cached->next;
    }

    for( ; cached != NULL; cached = cached->next) {
        if (((type == cached->rr.type) || (type == QTYPE_ANY)) &&
            (strcasecmp(cached->rr.name, host) == 0)) {
            return cached;
        }
    }
    return NULL;
}

/*!
 * \brief Get the next matching dns record in the published hash list
 *
 * Basic hash and linked list primatives for published dns record hash.
 * Iterate through the given dns record list and search for the given host.
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param cached    The dns record list head entry to start searching from or NULL
 *                  if a new search shall be started
 * \param host      host name to search for
 * \param type      query type
 *
 * \return          The next matching dns record or NULL
 */
static TMdnsdRecord *RecordNext(TMdnsd *mdnsd, TMdnsdRecord *record, uint8_t *host, int type)
{
    if (record == NULL) {
        record = mdnsd->published[NameHash(host) % SPRIME];
    } else {
        record = record->next;
    }

    for( ; record != NULL; record = record->next) {
        if (((type == record->rr.type) || (type == QTYPE_ANY)) &&
            (strcasecmp(record->rr.name, host) == 0)) {
            return record;
        }
    }
    return NULL;
}

/*!
 * \brief Get the length of the given ressource record
 *
 * \param rr        Ressource record buffer to calculate the length for
 *
 * \return          calculated length of the ressource record
 */
static int GetRessourceRecordLength(TMdnsdAnswer *rr)
{
    int len;

    /* Initialise the length: Name is always compressed (dup of earlier occurence)
       plus further normal stuff.
    */
    len = 12;

    if (rr->rdata)  len += rr->rdlen;
    if (rr->rdname) len += strlen(rr->rdname); /* add worst case */
    if (rr->ip.s_addr) len += 4;
    if (rr->type == QTYPE_PTR) len += 6; /* Add srv record length */

    return len;
}


/*!
 * \brief Compare the ressource data with a given answer record
 *
 * This is a painfull compare, with lots of needed comparisions... computing intensive
 *
 * \param res       ressource data to compare with the given answer
 * \param answer    Answer record to compare with
 *
 * \return          1 in case of a math or 0 if no match found
 */
static int MatchAnswer(DNSRESOURCE *res, TMdnsdAnswer *answer)
{
    /* Check if the name and ressource type is matching */
    if ((strcasecmp(res->name, answer->name) != 0) ||
        ((res->type != QTYPE_ANY) && (res->type != answer->type))) {
        /* no match ... */
        return 0;
    }

    /* If name matches, and ressource type is QTYPE_ANY we found a match */
    if ((res->type == QTYPE_ANY) && (strcasecmp(res->name, answer->name) == 0)) {
        return 1;
    }

    /* Special checks for SRV type ressource data */
    if ((res->type == QTYPE_SRV) && (strcasecmp(res->known.srv.name, answer->rdname) == 0) &&
        (answer->srv.port == res->known.srv.port) &&
        (answer->srv.weight == res->known.srv.weight) &&
        (answer->srv.priority == res->known.srv.priority)) {
        return 1;
    }

    /* Check for PTR, NS or CNAME type ressource data */
    if (((res->type == QTYPE_PTR) || (res->type == QTYPE_NS) || (res->type == QTYPE_CNAME)) &&
        (strcasecmp(answer->rdname, res->known.ns.name) == 0)) {
        return 1;
    }


    if ((res->rdlength == answer->rdlen) && (memcmp(res->rdata, answer->rdata, res->rdlength) == 0)) {
        return 1;
    }

    return 0;
}

/*!
 * \brief Calculate time elapsed between a and b
 *
 * Compares two timeval values
 *
 * \param a         older timeval value
 * \param b         newer timeval value
 *
 * \return          Elapsed time in µs
 */
static int TvDiff(struct timeval a, struct timeval b)
{
    int udiff = 0;

    if (a.tv_sec != b.tv_sec) {
        udiff = (b.tv_sec - a.tv_sec) * 1000000;
    }

    return (b.tv_usec - a.tv_usec) + udiff;
}

/*!
 * \brief create generic unicast response struct
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param record    Record to push
 * \param id        unicast record id
 */
static void MdnsdPushUnicast(TMdnsd *mdnsd, TMdnsdRecord *record, int id, struct in_addr to, uint16_t port)
{
    TUnicast *unicast;

    unicast = (TUnicast *)malloc(sizeof(TUnicast));
    memset(unicast, 0, sizeof(TUnicast));

    unicast->record = record;
    unicast->id = id;
    unicast->to = to;
    unicast->port = port;
    unicast->next = mdnsd->uanswers;

    mdnsd->uanswers = unicast;
}

/*!
 * \brief Insert a record to the list if not yet inserted
 *
 * \param list      Linked record list head
 * \param record    Record to insert
 */
static void MdnsdPushRecord(TMdnsdRecord **list, TMdnsdRecord *record)
{
    TMdnsdRecord *current;

    for(current = *list; current != NULL; current = current->list) {
        if(current == record) {
            return;
        }
    }

    record->list = *list;
    *list = record;
}

/*!
 * \brief Publish a record if valid
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param record    Record to publish
 */
static void MdnsdPublishRecord(TMdnsd *mdnsd, TMdnsdRecord *record)
{
    if (record->unique && (record->unique < 5)) {
        /* probing already */
        return;
    }

    record->tries = 0;
    mdnsd->publish.tv_sec = mdnsd->now.tv_sec;
    mdnsd->publish.tv_usec = mdnsd->now.tv_usec;

    MdnsdPushRecord(&mdnsd->a_publish, record);
}

/*!
 * \brief Send out a record as soon as possible
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param record    Record to send
 */
static void MdnsdSendRecord(TMdnsd *mdnsd, TMdnsdRecord *record)
{
    if (record->tries < 4) {
        /* The record has been published, speed up the things... */
        mdnsd->publish.tv_sec = mdnsd->now.tv_sec;
        mdnsd->publish.tv_usec = mdnsd->now.tv_usec;
        return;
    }

    if (record->unique) {
        /* Unique records can be sent ASAP */
        MdnsdPushRecord(&mdnsd->a_now, record);
        return;
    }
// TODO: better random, do not use modulo
    /* set mdnsd->pause.tv_usec to random 20-120 msec */
    mdnsd->pause.tv_sec = mdnsd->now.tv_sec;
    mdnsd->pause.tv_usec = mdnsd->now.tv_usec + ((mdnsd->now.tv_usec % 101) + 20) * 1000;
    if (mdnsd->pause.tv_usec >= 1000000) {
        mdnsd->pause.tv_sec++;
        mdnsd->pause.tv_usec -= 1000000;
    }

    /* And push the record... */
    MdnsdPushRecord(&mdnsd->a_pause, record);
}

/*!
 * \brief Clean up record
 *
 * Remove from hash and free allocated memory
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param record    Record to clean up
 */
static void MdnsdRecordDone(TMdnsd *mdnsd, TMdnsdRecord *record)
{
    TMdnsdRecord *current = NULL;
    int idx = NameHash(record->rr.name) % SPRIME;

    if(mdnsd->published[idx] == record) {
        mdnsd->published[idx] = record->next;
    } else {
        for (current = mdnsd->published[idx]; (current != NULL) && (current->next != record); current = current->next);

        if (current) {
            current->next = record->next;
        }
    }
    free(record->rr.name);
    free(record->rr.rdata);
    free(record->rr.rdname);
    free(record);
}

/*!
 * \brief Reset query
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param query     Query to reset
 */
static void MdnsdQueryReset(TMdnsd *mdnsd, TQuery *query)
{
    TCached *current = NULL;

    query->nexttry = 0;
    query->tries = 0;

    while ((current = CachedNext(mdnsd, current, query->name, query->type))) {
        if ((query->nexttry == 0 ) || (current->rr.ttl - 7 < query->nexttry)) {
            query->nexttry = current->rr.ttl - 7;
        }
    }

    if ((query->nexttry != 0) && (query->nexttry < mdnsd->checkqlist)) {
        mdnsd->checkqlist = query->nexttry;
    }
}

/*!
 * \brief Clean up query
 *
 * Update all its cached entries and remove it from list, and free allocated memory
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param query     Query to clean up
 */
static void MdnsdQueryDone(TMdnsd *mdnsd, TQuery *query)
{
    TCached *cached = NULL;
    TQuery  *current;
    int      idx;

    idx = NameHash(query->name) % SPRIME;

    while ((cached = CachedNext(mdnsd, cached, query->name, query->type))) {
        cached->query = NULL;
    }

    if (mdnsd->qlist == query) {
        mdnsd->qlist = query->list;
    } else {
        for (current = mdnsd->qlist; current->list != query; current = current->list);
        current->list = query->list;
    }

    if (mdnsd->queries[idx] == query) {
        mdnsd->queries[idx] = query->next;
    } else {
        for (current = mdnsd->queries[idx]; current->next != query; current = current->next);
        current->next = query->next;
    }

    free(query->name);
    free(query);
}

/*!
 * \brief call the answer function with this cached entry
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param cached    Cached record
 */
static void MdnsdQueryAnswer(TMdnsd *mdnsd, TCached *cached)
{
    if (cached->rr.ttl <= mdnsd->now.tv_sec) {
        cached->rr.ttl = 0;
    }

    if (cached->query->answer(&cached->rr, cached->query->arg) == -1) {
        MdnsdQueryDone(mdnsd, cached->query);
    }
}

/*!
 * \brief call the conflict function with this record
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param record    Record to call conflict for
 */
static void MdnsdCallConflict(TMdnsd *mdnsd, TMdnsdRecord *record)
{
    record->conflict(record, record->rr.name, record->rr.type, record->arg);
    MdnsdDone(mdnsd, record);
}

/*!
 * \brief Expire any old entries in this hash list
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param list      Cache hash list head
 */
static void MdnsdCacheExpire(TMdnsd *mdnsd, TCached **list)
{
    TCached *next;
    TCached *current  = *list;
    TCached *last = NULL;

    while(current != NULL) {
        next = current->next;

        if(mdnsd->now.tv_sec >= current->rr.ttl) {
            if (last) {
                last->next = next;
            }

            if (*list == current) {
                /* Update list pointer if the first one expired */
                *list = next;
            }

            if (current->query) {
                MdnsdQueryAnswer(mdnsd, current);
            }

            free(current->rr.name);
            free(current->rr.rdata);
            free(current->rr.rdname);
            free(current);
        } else {
            last = current;
        }
        current = next;
    }
}


/*!
 * \brief Garbage collector: Expire any old cached records
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 */
static void MdnsdCacheGarbageCollect(TMdnsd *mdnsd)
{
    int idx;

    for(idx = 0; idx < LPRIME; idx++) {
        if (mdnsd->cache[idx]) {
            MdnsdCacheExpire(mdnsd, &mdnsd->cache[idx]);
        }
    }

    mdnsd->expireall = mdnsd->now.tv_sec + GC;
}

/*!
 * \brief Add a ressource to the cache
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param res       ressource data to add
 */
static void MdnsdCacheAddRessource(TMdnsd *mdnsd, DNSRESOURCE *res)
{
    TCached *cached = NULL;

    int idx;

    idx = NameHash(res->name) % LPRIME;

    if (res->class == 32768 + mdnsd->class) {
        /* Flush the cache */
        while ((cached = CachedNext(mdnsd, cached, res->name, res->type))) {
            cached->rr.ttl = 0;
        }
        MdnsdCacheExpire(mdnsd, &mdnsd->cache[idx]);
    }

    if (res->ttl == 0) {
        /* Process deletes */
        while ((cached = CachedNext(mdnsd, cached, res->name, res->type))) {
            if (MatchAnswer(res, &cached->rr)) {
                cached->rr.ttl = 0;
            }
        }
        MdnsdCacheExpire(mdnsd, &mdnsd->cache[idx]);
        return;
    }

    cached = (TCached *)malloc(sizeof(TCached));
    memset(cached, 0, sizeof(TCached));

    cached->rr.name = strdup(res->name);
    cached->rr.type = res->type;
    cached->rr.ttl = mdnsd->now.tv_sec + (res->ttl / 2) + 8; // XXX hack for now, BAD SPEC, start retrying just after half-waypoint, then expire
    cached->rr.rdlen = res->rdlength;
    cached->rr.rdata = (uint8_t *)malloc(res->rdlength);
    memcpy(cached->rr.rdata, res->rdata, res->rdlength);

    switch(res->type) {
        case QTYPE_A:
            cached->rr.ip.s_addr = res->known.a.ip;
            break;
        case QTYPE_NS:
        case QTYPE_CNAME:
        case QTYPE_PTR:
            cached->rr.rdname = strdup(res->known.ns.name);
            break;
        case QTYPE_SRV:
            cached->rr.rdname = strdup(res->known.srv.name);
            cached->rr.srv.port = res->known.srv.port;
            cached->rr.srv.weight = res->known.srv.weight;
            cached->rr.srv.priority = res->known.srv.priority;
            break;
    }

    cached->next = mdnsd->cache[idx];
    mdnsd->cache[idx] = cached;

    if((cached->query = QueryNext(mdnsd, 0, res->name, res->type))) {
        MdnsdQueryAnswer(mdnsd, cached);
    }
}

/*!
 * \brief Copy an answer
 *
 * Copy the databits only
 *
 * \param msg       DNS message struct
 * \param answer    Answer to get the data from
 */
static void MdnsdCopyAnswer(DNSMESSAGE *msg, TMdnsdAnswer *answer)
{
    if(answer->rdata) {
        DnsMsgAdd_rdata_raw(msg, answer->rdata, answer->rdlen);
        return;
    }

    if(answer->ip.s_addr) {
        DnsMsgAdd_rdata_long(msg, answer->ip.s_addr);
    }

    if(answer->type == QTYPE_SRV) {
        DnsMsgAdd_rdata_srv(msg, answer->srv.priority, answer->srv.weight, answer->srv.port, answer->rdname);
    } else
    if (answer->rdname) {
        DnsMsgAdd_rdata_name(msg, answer->rdname);
    }
}

/*!
 * \brief Copy a published record into an outgoing message
 *
 * \param mdnsd     MDNS deamon data of the current mdnsd instace
 * \param msg       DNS message struct
 * \param list      List of publishing records
 *
 * \return          Number of send records
 */
static int MdnsdRecordOut(TMdnsd *mdnsd, DNSMESSAGE *m, TMdnsdRecord **list)
{
    TMdnsdRecord *record;
    int ret = 0;

    while (((record = *list) != NULL) &&
           (DnsMsgLen(m) + GetRessourceRecordLength(&record->rr) < mdnsd->frame)) {

        *list = record->list;
        ret++;

        if (record->unique) {
            DnsMsgAdd_an(m, record->rr.name, record->rr.type, mdnsd->class + 32768, record->rr.ttl);
        } else {
            DnsMsgAdd_an(m, record->rr.name, record->rr.type, mdnsd->class, record->rr.ttl);
        }
        MdnsdCopyAnswer(m, &record->rr);

        if(record->rr.ttl == 0) {
            MdnsdRecordDone(mdnsd, record);
        }
    }
    return ret;
}

/*!
 * \brief Initialise the mdnsd deamon instance
 *
 * Create a new mdns daemon for the given class of names (usually 1) and maximum frame size
 *
 * \param class     Class of names
 * \param frame     Maximum frame size
 *
 * \return          Newly allocated mdnsd struct instance
 */
TMdnsd *MdnsdNew(int class, int frame)
{
    TMdnsd *mdnsd;

    mdnsd = (TMdnsd*)malloc(sizeof(TMdnsd));
    memset(mdnsd, 0, sizeof(TMdnsd));

    gettimeofday(&mdnsd->now, 0);
    mdnsd->expireall = mdnsd->now.tv_sec + GC;

    mdnsd->class = class;
    mdnsd->frame = frame;

    return mdnsd;
}

/*!
 * \brief Shutdown and cleanup an MDNSD instance
 *
 * Gracefully shutdown the daemon, use mdnsd_out() to get the last packets
 *
 * \param mdnsd     MDNS deamon to shutdown
 *
 * \return          Newly allocated mdnsd struct instance
 */
void MdnsdShutdown(TMdnsd *mdnsd)
{

    int idx;
    TMdnsdRecord *current;
    TMdnsdRecord *next;

    mdnsd->a_now = 0;

    /* zero out ttl and push out all records */
    for(idx = 0; idx < SPRIME; idx++) {
        for(current = mdnsd->published[idx]; current != NULL; ) {
            next = current->next;
            current->rr.ttl = 0;
            current->list = mdnsd->a_now;
            mdnsd->a_now = current;
            current = next;
        }
    }

    mdnsd->shutdown = 1;
}


/*!
 * \brief Flush all cached records (network/interface changed)
 *
 * \param mdnsd     MDNS deamon to flush
 */
void MdnsdFlush(TMdnsd *UNUSED(mdnsd))
{
    // TODO:
    // set all querys to 0 tries
    // free whole cache
    // set all TMdnsdRecord *to probing
    // reset all answer lists
}

/*!
 * \brief Free given mdnsd (should have used mdnsd_shutdown() first!)
 *
 * \param mdnsd     MDNS deamon to free
 */
void MdnsdFree(TMdnsd *mdnsd)
{
    // TODO:
    // loop through all hashes, free everything
    // free answers if any
    free(mdnsd);
}

#ifdef MDNSD_DEBUG
uint8_t *MdnsdDecodeType(uint16_t type)
{
    switch (type) {
        case QTYPE_A:     return "A";
        case QTYPE_NS:    return "NS";
        case QTYPE_CNAME: return "CNAME";
        case QTYPE_PTR:   return "PTR";
        case QTYPE_TXT:   return "TXT";
        case QTYPE_SRV:   return "SRV";
        default:          return "???";
    }
}

void MdnsdDumpRessource (FILE *file, DNSRESOURCE *res)
{
    fprintf (file, "%s \"%s\" = ", MdnsdDecodeType (res->type), res->name);

    switch (res->type) {
        case QTYPE_A:
            fprintf (file, "%d.%d.%d.%d\n",
                     (res->known.a.ip >> 24) & 0xff,
                     (res->known.a.ip >> 16) & 0xff,
                     (res->known.a.ip >> 8) & 0xff,
                     (res->known.a.ip >> 0) & 0xff);
            break;

        case QTYPE_NS:
            fprintf (file, "%s\n", res->known.ns.name);
            break;

        case QTYPE_CNAME:
            fprintf (file, "%s\n", res->known.cname.name);
            break;

        case QTYPE_PTR:
            fprintf (file, "%s\n", res->known.ptr.name);
            break;

        case QTYPE_SRV:
            fprintf (file, "%s:%d\n", res->known.srv.name, res->known.srv.port);
            break;

        default:
            fprintf (file, "???\n");
    }
}

void mdnsd_dump (FILE *file, DNSMESSAGE *msg, uint8_t *type)
{
    int idx;

    fprintf (file, "==== %s message ====\n", type);

    if ((msg->header.qr == 0) && (msg->qdcount > 0)) {
        fprintf (file, "Questions:\n");
        for (idx = 0; idx < msg->qdcount; idx++) {
            fprintf (file, " %3d: %s \"%s\"?\n", idx,
                     MdnsdDecodeType (msg->qd[idx].type), msg->qd[idx].name);
        }
    }

    if (msg->ancount > 0) {
        fprintf (file, "Answers:\n");
        for (idx = 0; idx < msg->ancount; idx++) {
            fprintf (file, " %3d: ", idx);
            MdnsdDumpRessource (file, &msg->an[idx]);
        }
    }

    if (msg->nscount > 0) {
        fprintf (file, "Authority:\n");
        for (idx = 0; idx < msg->nscount; idx++) {
            fprintf (file, " %3d: ", idx);
            MdnsdDumpRessource (file, &msg->ns[idx]);
        }
    }
    if (msg->arcount > 0) {
        fprintf (file, "Additional:\n");
        for (idx = 0; idx < msg->arcount; idx++) {
            fprintf (file, " %3d: ", idx);
            MdnsdDumpRessource (file, &msg->ar[idx]);
        }
    }
    fprintf (file, "\n");
}
#endif /* MDNSD_DEBUG */


/*******************************************************************************
 *                            I/O functions                                    *
 *******************************************************************************/

/*!
 * \brief Process incomming messages from the host
 *
 * This function processes each query and sends out the matching unicast reply
 * to each query. For each question, the potential answers are checked. Each
 * answer is checked for potential conflicts. Each answer is processed and
 * cached.
 *
 * \param mdnsd     MDNS deamon instance
 * \param msg       incomming message
 * \param ip        source IP
 * \param port      source port
 *
 */

// TODO: SRV record: "reinhardt" is cut down to "einhardt", first character is dropped... Wrong index?

void MdnsdInput(TMdnsd *mdnsd, DNSMESSAGE *msg, struct in_addr ip, uint16_t port)
{
    int qd_idx;
    int an_idx;
    TMdnsdRecord *record;
    int have_match;
    int may_conflict;

    if(mdnsd->shutdown) return;

    gettimeofday(&mdnsd->now,0);

    if(msg->header.qr == 0) {
        /* This message contains a query... Process the question and send out
           our answer if needed
         */

        for(qd_idx = 0; qd_idx < msg->qdcount; qd_idx++) {
            /* Process each query */
            if ((msg->qd[qd_idx].class != mdnsd->class) ||
                ((record = RecordNext(mdnsd, 0, msg->qd[qd_idx].name, msg->qd[qd_idx].type)) == NULL)) {
                continue;
            }

            /* Send the matching unicast reply */
            if (port != MDNS_PORT) {
                MdnsdPushUnicast(mdnsd, record, msg->id, ip, port);
            }

            for( ; record != NULL; record = RecordNext(mdnsd, record, msg->qd[qd_idx].name, msg->qd[qd_idx].type)) {
                /* Check all of our potential answers */
                have_match   = 0;
                may_conflict = 0;

                if (record->unique && record->unique < 5) {
                    /* Probing state, check for conflicts */

                    for(an_idx = 0; an_idx < msg->nscount; an_idx++) {
                        /* Check all to-be answers against our own */
                        if ((msg->an[an_idx].ttl == 0) || (msg->qd[qd_idx].type != msg->an[an_idx].type) ||
                            (strcasecmp(msg->qd[qd_idx].name, msg->an[an_idx].name) != 0)) {
                            continue;
                        }

                        if (!MatchAnswer(&msg->an[an_idx], &record->rr)) {
                            /* Not matching answers may cause conflicts */
                            may_conflict = 1;
                        } else {
                            have_match = 1;
                        }
                    }

                    if (may_conflict && !have_match) {
                        /* The answer isn't ours, we have a conflict */
                        MdnsdCallConflict(mdnsd, record);
                    }

                    continue;
                }

                for(an_idx = 0; an_idx < msg->ancount; an_idx++) {
                    /* Check the known answers for this question */
                    if (((msg->qd[qd_idx].type != QTYPE_ANY) && (msg->qd[qd_idx].type != msg->an[an_idx].type)) ||
                        (strcasecmp(msg->qd[qd_idx].name, msg->an[an_idx].name) != 0)) {
                        continue;
                    }

                    if (MatchAnswer(&msg->an[an_idx], &record->rr)) {
                        /* They already have this answer */
                        break;
                    }
                }

                if(an_idx == msg->ancount) {
                    /* No matching answers found, send out our answer */
                    MdnsdSendRecord(mdnsd, record);
                }
            }
        }
        return;
    }

    for (an_idx = 0; an_idx < msg->ancount; an_idx++) {
        /* Process each answer, check for a conflict, and cache it */
        have_match = 0;
        may_conflict = 0;

        record = NULL;
        while ((record = RecordNext(mdnsd, record, msg->an[an_idx].name, msg->an[an_idx].type)) != NULL) {
            if (record->unique) {
                if (MatchAnswer(&msg->an[an_idx], &record->rr) == 0) {
                    may_conflict = 1;
                } else {
                    have_match = 1;
                }
            }
        }

        if (may_conflict && !have_match) {
            while ((record = RecordNext(mdnsd, record, msg->an[an_idx].name, msg->an[an_idx].type)) != NULL) {
                if ((record->unique && MatchAnswer(&msg->an[an_idx], &record->rr) == 0) && (msg->an[an_idx].ttl > 0)) {
                    MdnsdCallConflict(mdnsd, record);
                }
            }
        }

        MdnsdCacheAddRessource(mdnsd, &msg->an[an_idx]);
    }
}

/*!
 * \brief Send outgoing messages to the host.
 *
 * \param mdnsd     MDNS deamon instance
 * \param msg       outgoing message
 * \param ip        destination IP
 * \param port      destination port
 *
 * \return          >0 if one was returned and m/ip/port set
 */
int MdnsdOutput(TMdnsd *mdnsd, DNSMESSAGE *msg, struct in_addr *ip, uint16_t *port)
{
    TMdnsdRecord *record;
    int ret = 0;

    gettimeofday(&mdnsd->now,0);
    memset(msg, 0, sizeof(DNSMESSAGE));

    /* Set multicast defaults */
    *port = htons(MDNS_PORT);
    (*ip).s_addr = inet_addr(MDNS_MULTICAST_IP);
    msg->header.qr = 1;
    msg->header.aa = 1;

    if(mdnsd->uanswers) {
        /* Send out individual unicast answers */
        TUnicast *unicast = mdnsd->uanswers;

        mdnsd->uanswers = unicast->next;
        *port = unicast->port;
        *ip = unicast->to;
        msg->id = unicast->id;

        DnsMsgAdd_qd(msg, unicast->record->rr.name, unicast->record->rr.type, mdnsd->class);
        DnsMsgAdd_an(msg, unicast->record->rr.name, unicast->record->rr.type, mdnsd->class, unicast->record->rr.ttl);

        MdnsdCopyAnswer(msg, &unicast->record->rr);

        free(unicast);
        return 1;
    }

    //printf("OUT: probing %p now %p pause %p publish %p\n",mdnsd->probing,mdnsd->a_now,mdnsd->a_pause,mdnsd->a_publish);

    /* Accumulate any immediate responses */
    if (mdnsd->a_now) {
        ret += MdnsdRecordOut(mdnsd, msg, &mdnsd->a_now);
    }

    if (mdnsd->a_publish && (TvDiff(mdnsd->now,mdnsd->publish) <= 0)) {
        /* Check to see if it's time to send the publish retries (and unlink if done) */
        TMdnsdRecord *next;
        TMdnsdRecord *current;
        TMdnsdRecord *last = NULL;

        current = mdnsd->a_publish;
        while(current && (DnsMsgLen(msg) + GetRessourceRecordLength(&current->rr) < mdnsd->frame)) {
            next = current->list;
            ret++;
            current->tries++;

            if (current->unique) {
                DnsMsgAdd_an(msg, current->rr.name, current->rr.type, mdnsd->class + 32768, current->rr.ttl);
            } else {
                DnsMsgAdd_an(msg, current->rr.name, current->rr.type, mdnsd->class, current->rr.ttl);
            }
            MdnsdCopyAnswer(msg, &current->rr);

            if ((current->rr.ttl != 0) && (current->tries < 4)) {
                last = current;
                current = next;
                continue;
            }

            if (mdnsd->a_publish == current) {
                mdnsd->a_publish = next;
            }

            if (last) {
                last->list = next;
            }

            if (current->rr.ttl == 0) {
                MdnsdRecordDone(mdnsd, current);
            }
            current = next;
        }

        if (mdnsd->a_publish) {
            mdnsd->publish.tv_sec = mdnsd->now.tv_sec + 2;
            mdnsd->publish.tv_usec = mdnsd->now.tv_usec;
        }
    }

    /* If we're in shutdown state, we're done */
    if (mdnsd->shutdown) {
        return ret;
    }

    /* Check if a_pause is ready */
    if (mdnsd->a_pause && (TvDiff(mdnsd->now, mdnsd->pause) <= 0)) {
        ret += MdnsdRecordOut(mdnsd, msg, &mdnsd->a_pause);
    }

    /* Now process questions */
    if (ret) {
        return ret;
    }

    msg->header.qr = 0;
    msg->header.aa = 0;

    if (mdnsd->probing && (TvDiff(mdnsd->now, mdnsd->probe) <= 0)) {
        TMdnsdRecord *last = NULL;

        for (record = mdnsd->probing; record != NULL; ) {
            /* Scan probe list to ask questions and process published */
            if (record->unique == 4) {
                /* Done probing, publish now */
                TMdnsdRecord *next = record->list;

                if (mdnsd->probing == record) {
                    mdnsd->probing = record->list;
                } else {
                    last->list = record->list;
                }

                record->list = NULL;
                record->unique = 5;

                MdnsdPublishRecord(mdnsd, record);

                record = next;
                continue;
            }
            DnsMsgAdd_qd(msg, record->rr.name, QTYPE_ANY, mdnsd->class);
            last = record;
            record = record->list;
        }

        for (record = mdnsd->probing; record != NULL; last = record, record = record->list) {
            /* Scan probe list again to append our to-be answers */
            record->unique++;
            DnsMsgAdd_ns(msg, record->rr.name, record->rr.type, mdnsd->class, record->rr.ttl);
            MdnsdCopyAnswer(msg, &record->rr);
            ret++;
        }

        if (ret) {
            /* Set timeout to process probes again */
            mdnsd->probe.tv_sec = mdnsd->now.tv_sec;
            mdnsd->probe.tv_usec = mdnsd->now.tv_usec + 250000;
            return ret;
        }
    }

    if (mdnsd->checkqlist && (mdnsd->now.tv_sec >= mdnsd->checkqlist)) {
        /* Process qlist for retries or expirations */
        TQuery *query;
        TCached *cached;
        uint32_t nextbest = 0;

        /* Ask questions first, track nextbest time */
        for(query = mdnsd->qlist; query != NULL; query = query->list) {
            if ((query->nexttry > 0) && (query->nexttry <= mdnsd->now.tv_sec) && (query->tries < 3)) {
                DnsMsgAdd_qd(msg, query->name, query->type,mdnsd->class);
            } else
            if ((query->nexttry > 0) && ((nextbest == 0) || (query->nexttry < nextbest))) {
                nextbest = query->nexttry;
            }
        }

        /* Include known answers, update questions */
        for (query = mdnsd->qlist; query != NULL; query = query->list) {
            if ((query->nexttry == 0) || (query->nexttry > mdnsd->now.tv_sec)) {
                continue;
            }

            if (query->tries == 3) {
                /* Done retrying, expire and reset */
                MdnsdCacheExpire(mdnsd, &mdnsd->cache[NameHash(query->name) % LPRIME]);
                MdnsdQueryReset(mdnsd, query);
                continue;
            }

            ret++;
            query->nexttry = mdnsd->now.tv_sec + ++query->tries;

            if ((nextbest == 0) || (query->nexttry < nextbest)) {
                nextbest = query->nexttry;
            }

            /* If room, add all known good entries */
            cached = NULL;
            while (((cached = CachedNext(mdnsd, cached, query->name, query->type)) != NULL) &&
                   (cached->rr.ttl > mdnsd->now.tv_sec + 8) && (DnsMsgLen(msg) + GetRessourceRecordLength(&cached->rr) < mdnsd->frame)) {
                DnsMsgAdd_an(msg, query->name, query->type, mdnsd->class, cached->rr.ttl - mdnsd->now.tv_sec);
                MdnsdCopyAnswer(msg, &cached->rr);
            }
        }

        mdnsd->checkqlist = nextbest;
    }

    if (mdnsd->now.tv_sec > mdnsd->expireall) {
        MdnsdCacheGarbageCollect(mdnsd);
    }

    return ret;
}


/*!
 * \brief Send outgoing messages to the host.
 *
 * This function returns the max wait-time until MdnsdOutput() needs to be
 * called again
 *
 * \param mdnsd     MDNS deamon instance
 *
 * \return          Maximum time after which MdnsdOutput needs to be called again
 */
struct timeval *MdnsdGetMaxSleepTime(TMdnsd *mdnsd)
{
    int sec, usec;
    mdnsd->sleep.tv_sec = mdnsd->sleep.tv_usec = 0;

    /* first check for any immediate items to handle */
    if(mdnsd->uanswers || mdnsd->a_now) {
        return &mdnsd->sleep;
    }

    gettimeofday(&mdnsd->now,0);

    if(mdnsd->a_pause) {
        /* Check for paused answers */
        if ((usec = TvDiff(mdnsd->now,mdnsd->pause)) > 0) {
            mdnsd->sleep.tv_usec = usec;
        }
        goto out;
    }

    if(mdnsd->probing) {
        /* Check for probe retries */
        if ((usec = TvDiff(mdnsd->now,mdnsd->probe)) > 0) {
            mdnsd->sleep.tv_usec = usec;
        }
        goto out;
    }

    if(mdnsd->a_publish) {
        /* Check for publish retries */
        if ((usec = TvDiff(mdnsd->now,mdnsd->publish)) > 0) {
            mdnsd->sleep.tv_usec = usec;
        }
        goto out;
    }

    if(mdnsd->checkqlist) {
        /* Also check for queries with known answer expiration/retry */
        if ((sec = mdnsd->checkqlist - mdnsd->now.tv_sec) > 0) {
            mdnsd->sleep.tv_sec = sec;
        }
        goto out;
    }

    /* Last resort, next gc expiration */
    if ((sec = mdnsd->expireall - mdnsd->now.tv_sec) > 0) {
        mdnsd->sleep.tv_sec = sec;
    }

out:
    /* Fix up seconds ... */
    while (mdnsd->sleep.tv_usec > 1000000) {
        mdnsd->sleep.tv_sec++;
        mdnsd->sleep.tv_usec -= 1000000;
    }
    return &mdnsd->sleep;
}

/*******************************************************************************
 *                       Query and answer functions                            *
 *******************************************************************************/

/*!
 * \brief Register a new query
 *
 * Answer(record, arg) is called whenever one is found/changes/expires
 * (immediate or anytime after, mdnsda valid until ->ttl==0)
 * Either answer returns -1, or another MdnsdQuery with a NULL answer will
 * remove/unregister this query
 * This function returns the max wait-time until MdnsdOutput() needs to be
 * called again
 *
 * \param mdnsd     MDNS deamon instance
 * \param host      Hostname
 * \param type      Query type
 * \param answer    Callback to the answer function, which shall be called if an
 *                  answer was found / changes / expires...
 *
 * \return          Maximum time after which MdnsdOutput needs to be called again
 */
void MdnsdQuery(TMdnsd *mdnsd, uint8_t *host, int type, int (*answer)(TMdnsdAnswer *answer, void *arg), void *arg)
{
    TQuery  *query;
    TCached *current = NULL;
    int      idx;

    idx = NameHash(host) % SPRIME;
    if ((query = QueryNext(mdnsd, 0, host,type)) == NULL) {
        if (answer == NULL) {
            return;
        }

        query = (TQuery *)malloc(sizeof(TQuery));
        memset(query, 0, sizeof(TQuery));

        query->name = strdup(host);
        query->type = type;
        query->next = mdnsd->queries[idx];
        query->list = mdnsd->qlist;
        mdnsd->qlist = mdnsd->queries[idx] = query;

        while ((current = CachedNext(mdnsd, current, query->name, query->type))) {
            /* Any cached entries should be associated */
            current->query = query;
        }

        MdnsdQueryReset(mdnsd, query);

        /* New questin, immediately send out */
        query->nexttry = mdnsd->checkqlist = mdnsd->now.tv_sec;
    }

    if (answer == NULL) {
        /* no answer means we don't care anymore */
        MdnsdQueryDone(mdnsd, query);
        return;
    }

    query->answer = answer;
    query->arg = arg;
}

/*!
 * \brief Get the next answer from the cache
 *
 * Returns the first (if last == NULL) or next answer after last from the cache
 *
 * \param mdnsd     MDNS deamon instance
 * \param host      Hostname
 * \param type      Query type
 * \param last      Last answer
 *
 * \return          next cached answer
 */
TMdnsdAnswer *MdnsdListCachedAnswers(TMdnsd *mdnsd, uint8_t *host, int type, TMdnsdAnswer *last)
{
    return (TMdnsdAnswer *)CachedNext(mdnsd, (TCached *)last, host, type);
}

/*******************************************************************************
 *                        Publishing functions                                 *
 *******************************************************************************/

/*!
 * \brief Create a new shared record
 *
 * Returns the newly allocated record struct
 *
 * \param mdnsd     MDNS deamon instance
 * \param host      Hostname
 * \param type      Query type
 * \param ttl       Time to live value
 *
 * \return          newly allocated share record
 */
TMdnsdRecord *MdnsdAllocShared(TMdnsd *mdnsd, uint8_t *host, int type, uint32_t ttl)
{
    int idx;
    TMdnsdRecord *record;

    idx = NameHash(host) % SPRIME;

    record = (TMdnsdRecord *)malloc(sizeof(TMdnsdRecord));
    memset(record, 0, sizeof(TMdnsdRecord));

    record->rr.name = strdup(host);
    record->rr.type = type;
    record->rr.ttl = ttl;
    record->next = mdnsd->published[idx];

    mdnsd->published[idx] = record;

    return record;
}


/*!
 * \brief Create a new unique record
 *
 * Create a new unique record (try MdnsdListCachedAnswers first to make sure
 * it's not used)
 *
 * The conflict callback will be called at any point when one is detected and
 * is unable to recover.
 *
 * After the first data is set by MdnsdSet*(), any future changes effectively
 * expire the old one and attempt to create a new unique record.
 *
 * \param mdnsd     MDNS deamon instance
 * \param host      Hostname
 * \param type      Query type
 * \param ttl       Time to live value
 * \param conflict  Callback function called in case of a conflict
 * \param arg       Argument passed to the conflict callback
 *
 * \return          newly allocated share record
 */
TMdnsdRecord *MdnsdAllocUnique(TMdnsd *mdnsd, uint8_t *host, int type, uint32_t ttl, void (*conflict)(TMdnsdRecord *record, uint8_t *host, int type, void *arg), void *arg)
{
    TMdnsdRecord *record;
    record = MdnsdAllocShared(mdnsd, host, type, ttl);
    record->conflict = conflict;
    record->arg = arg;
    record->unique = 1;
    MdnsdPushRecord(&mdnsd->probing, record);
    mdnsd->probe.tv_sec = mdnsd->now.tv_sec;
    mdnsd->probe.tv_usec = mdnsd->now.tv_usec;
    return record;
}


/*!
 * \brief Remove record from the list and clean up
 *
 * \param mdnsd     MDNS deamon instance
 * \param record    The record which shall be de-listed
 *
 * \return          newly allocated share record
 */
void MdnsdDone(TMdnsd *mdnsd, TMdnsdRecord *record)
{
    TMdnsdRecord *cur;
    if(record->unique && record->unique < 5)
    { // probing yet, zap from that list first!
        if(mdnsd->probing == record) {
            mdnsd->probing = record->list;
        } else {
            for (cur=mdnsd->probing; cur->list != record; cur = cur->list);
            cur->list = record->list;
        }
        MdnsdRecordDone(mdnsd, record);
        return;
    }
    record->rr.ttl = 0;
    MdnsdSendRecord(mdnsd, record);
}


/*!
 * \brief Set/update raw data of the record and call publish
 *
 * \param mdnsd     MDNS deamon instance
 * \param record    The record which shall be de-listed
 * \param data      Raw record data
 * \param len       Datalength
 */
void MdnsdSetRaw(TMdnsd *mdnsd, TMdnsdRecord *record, uint8_t *data, int len)
{
    free(record->rr.rdata);
    record->rr.rdata = (uint8_t *)malloc(len);
    memcpy(record->rr.rdata,data,len);
    record->rr.rdlen = len;
    MdnsdPublishRecord(mdnsd, record);
}


/*!
 * \brief Set/update record host entry and call publish
 *
 * \param mdnsd     MDNS deamon instance
 * \param record    The record which shall be de-listed
 * \param name      Hostname
 */
void MdnsdSetHost(TMdnsd *mdnsd, TMdnsdRecord *record, uint8_t *name)
{
    free(record->rr.rdname);
    record->rr.rdname = strdup(name);
    MdnsdPublishRecord(mdnsd, record);
}


/*!
 * \brief Set/update IP address entry and call publish
 *
 * \param mdnsd     MDNS deamon instance
 * \param record    The record which shall be de-listed
 * \param ip        IP address
 */
void MdnsdSetIp(TMdnsd *mdnsd, TMdnsdRecord *record, struct in_addr ip)
{
    record->rr.ip = ip;
    MdnsdPublishRecord(mdnsd, record);
}


/*!
 * \brief Set/update service info and call publish
 *
 * \param mdnsd     MDNS deamon instance
 * \param record    The record which shall be de-listed
 * \param priority  Priority of the target host: lower value means more preferred.
 * \param weight    Relative weight for records with the same priority.
 * \param port      TCP / UDP port number of the service
 * \param name      The canonical hostname of the machine providing the service.
 */
void MdnsdSetSrv(TMdnsd *mdnsd, TMdnsdRecord *record, int priority, int weight, uint16_t port, uint8_t *name)
{
    record->rr.srv.priority = priority;
    record->rr.srv.weight = weight;
    record->rr.srv.port = port;
    MdnsdSetHost(mdnsd, record, name);
}

/*@}*/
