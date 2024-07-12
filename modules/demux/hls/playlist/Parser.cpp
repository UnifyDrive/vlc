/*
 * Parser.cpp
 *****************************************************************************
 * Copyright © 2015 - VideoLAN and VLC Authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "Parser.hpp"
#include "HLSSegment.hpp"
#include "HLSRepresentation.hpp"
#include "../../adaptive/SharedResources.hpp"
#include "../../adaptive/playlist/BasePeriod.h"
#include "../../adaptive/playlist/BaseAdaptationSet.h"
#include "../../adaptive/playlist/SegmentList.h"
#include "../../adaptive/encryption/Keyring.hpp"
#include "../../adaptive/tools/Retrieve.hpp"
#include "../../adaptive/tools/Helper.h"
#include "../../adaptive/tools/Conversions.hpp"
#include "M3U8.hpp"
#include "Tags.hpp"

#include <vlc_strings.h>
#include <vlc_stream.h>
#include <cstdio>
#include <sstream>
#include <map>
#include <cctype>
#include <algorithm>
#include <limits>

using namespace adaptive;
using namespace adaptive::playlist;
using namespace hls::playlist;

M3U8Parser::M3U8Parser(SharedResources *res)
{
    resources = res;
}

M3U8Parser::~M3U8Parser   ()
{
}

static std::list<Tag *> getTagsFromList(std::list<Tag *> &list, int tag)
{
    std::list<Tag *> ret;
    std::list<Tag *>::const_iterator it;
    for(it = list.begin(); it != list.end(); ++it)
    {
        if( (*it)->getType() == tag )
            ret.push_back(*it);
    }
    return ret;
}

static Tag * getTagFromList(std::list<Tag *> &list, int tag)
{
    std::list<Tag *>::const_iterator it;
    for(it = list.begin(); it != list.end(); ++it)
    {
        if( (*it)->getType() == tag )
            return *it;
    }
    return nullptr;
}

static void releaseTagsList(std::list<Tag *> &list)
{
    std::list<Tag *>::const_iterator it;
    for(it = list.begin(); it != list.end(); ++it)
        delete *it;
    list.clear();
}

HLSRepresentation * M3U8Parser::createRepresentation(BaseAdaptationSet *adaptSet, const AttributesTag * tag, vlc_object_t *p_obj)
{
    const Attribute *uriAttr = tag->getAttributeByName("URI");
    const Attribute *bwAttr = tag->getAttributeByName("BANDWIDTH");
    const Attribute *resAttr = tag->getAttributeByName("RESOLUTION");

    HLSRepresentation *rep = new (std::nothrow) HLSRepresentation(adaptSet);
    if(rep)
    {
        if(uriAttr)
        {
            std::string uri;
            if(tag->getType() == AttributesTag::EXTXMEDIA)
            {
                uri = uriAttr->quotedString();
            }
            else
            {
                uri = uriAttr->value;
            }
            if (p_obj)
                    msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: uri[%s].", __FILE__ , __FUNCTION__, __LINE__, uri.c_str());
            rep->setID(uri);
            rep->setPlaylistUrl(uri);
            if(uri.find('/') != std::string::npos)
            {
                uri = Helper::getDirectoryPath(uri);
                if(!uri.empty())
                    rep->baseUrl.Set(new Url(uri.append("/")));
            }
        }

        if(bwAttr)
            rep->setBandwidth(bwAttr->decimal());

        if(tag->getAttributeByName("CODECS"))
            rep->addCodecs(tag->getAttributeByName("CODECS")->quotedString());

        if(resAttr)
        {
            std::pair<int, int> res = resAttr->getResolution();
            if(res.first && res.second)
            {
                rep->setWidth(res.first);
                rep->setHeight(res.second);
            }
        }

        const Attribute *rateAttr = tag->getAttributeByName("FRAME-RATE");
        if(rateAttr)
        {
            unsigned num, den;
            vlc_ureduce(&num, &den, rateAttr->floatingPoint() * 1000, 1000, 0);
            rep->setFrameRate(Rate(num, den));
        }
    }

    return rep;
}

void M3U8Parser::createAndFillRepresentation(vlc_object_t *p_obj, BaseAdaptationSet *adaptSet,
                                             const AttributesTag *tag,
                                             const std::list<Tag *> &tagslist)
{
    HLSRepresentation *rep  = createRepresentation(adaptSet, tag, p_obj);
    if(rep)
    {
        parseSegments(p_obj, rep, tagslist, true);
        adaptSet->addRepresentation(rep);
    }
}

bool M3U8Parser::appendSegmentsFromPlaylistURI(vlc_object_t *p_obj, HLSRepresentation *rep)
{
    block_t *p_block = Retrieve::HTTP(resources, ChunkType::Playlist, rep->getPlaylistUrl().toString());
    if(p_block)
    {
        stream_t *substream = vlc_stream_MemoryNew(p_obj, p_block->p_buffer, p_block->i_buffer, true);
        if(substream)
        {
            std::list<Tag *> tagslist = parseEntries(substream);
            vlc_stream_Delete(substream);

            parseSegments(p_obj, rep, tagslist);

            releaseTagsList(tagslist);
        }
        block_Release(p_block);
        return true;
    }
    return false;
}

static bool parseEncryption(const AttributesTag *keytag, const Url &playlistUrl,
                            CommonEncryption &encryption)
{
    if( keytag->getAttributeByName("METHOD") &&
        keytag->getAttributeByName("METHOD")->value == "AES-128" &&
        keytag->getAttributeByName("URI") )
    {
        encryption.method = CommonEncryption::Method::AES_128;
        encryption.uri.clear();

        Url keyurl(keytag->getAttributeByName("URI")->quotedString());
        if(!keyurl.hasScheme())
        {
            keyurl.prepend(Helper::getDirectoryPath(playlistUrl.toString()).append("/"));
        }

        encryption.uri = keyurl.toString();

        if(keytag->getAttributeByName("IV"))
        {
            encryption.iv.clear();
            encryption.iv = keytag->getAttributeByName("IV")->hexSequence();
        }
        return true;
    }
    else
    {
        /* unsupported or invalid */
        encryption.method = CommonEncryption::Method::None;
        encryption.uri.clear();
        encryption.iv.clear();
        return false;
    }
}

void M3U8Parser::parseSegments(vlc_object_t *p_obj, HLSRepresentation *rep, const std::list<Tag *> &tagslist, bool first)
{
    SegmentList *segmentList = new (std::nothrow) SegmentList(rep);

    Timescale timescale(1000000);
    rep->addAttribute(new TimescaleAttr(timescale));
    rep->b_loaded = true;

    mtime_t totalduration = 0;
    mtime_t nzStartTime = 0;
    mtime_t absReferenceTime = VLC_TS_INVALID;
    mtime_t partTarget = 0;
    mtime_t partduration = 0;
    int     partmp4_numbers = 0;
    uint64_t sequenceNumber = 0;
    bool discontinuity = false;
    bool partIndependent = false;
    std::size_t prevbyterangeoffset = 0;
    const SingleValueTag *ctx_byterange = nullptr;
    CommonEncryption encryption;
    const ValuesListTag *ctx_extinf = nullptr;
    int useLLhlsLive = 0;

    if(p_obj)
        useLLhlsLive = var_InheritInteger (p_obj, "use-llhls-live");

    std::list<HLSSegment *> segmentstoappend;

    std::list<Tag *>::const_iterator it;
    for(it = tagslist.begin(); it != tagslist.end(); ++it)
    {
        const Tag *tag = *it;
        partduration = 0;
        partIndependent = false;
        switch(tag->getType())
        {
            /* using static cast as attribute type permits avoiding class check */
            case SingleValueTag::EXTXMEDIASEQUENCE:
            {
                sequenceNumber = (static_cast<const SingleValueTag*>(tag))->getValue().decimal();
            }
            break;

            case ValuesListTag::EXTINF:
            {
                ctx_extinf = static_cast<const ValuesListTag *>(tag);
            }
            break;

            case AttributesTag::EXTXPARTINF:
            {
                if(useLLhlsLive == 0){
                    if (p_obj)
                        msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Do not run useLLhlsLive.", __FILE__ , __FUNCTION__, __LINE__);
                    break;
                }
                const AttributesTag *keytag = static_cast<const AttributesTag *>(tag);
                const Attribute *partTargetAttr;
                if(keytag && (partTargetAttr = keytag->getAttributeByName("PART-TARGET"))) 
                {
                    if(partTargetAttr){
                        partTarget = CLOCK_FREQ * partTargetAttr->floatingPoint();
                        rep->parttargetDuration = partTarget;
                    }
                    keytag = nullptr;
                    if (p_obj && 0)
                        msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Find PART-TARGET[%lld].", __FILE__ , __FUNCTION__, __LINE__, partTarget);
                }
            }
            break;

            case AttributesTag::EXTXPART:
            {
                if(useLLhlsLive == 0){
                    if (p_obj)
                        msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Do not run useLLhlsLive.", __FILE__ , __FUNCTION__, __LINE__);
                    break;
                }
                const AttributesTag *keytag = static_cast<const AttributesTag *>(tag);
                const Attribute *partDurationAttr = NULL;
                const Attribute *partUriAttr = NULL;
                const Attribute *partIndependentAttr = NULL;
                std::string uri;
                if(keytag)
                {
                    partDurationAttr = keytag->getAttributeByName("DURATION");
                    if(partDurationAttr) {
                        partduration = CLOCK_FREQ * partDurationAttr->floatingPoint();
                        if(partduration > 0){
                            partmp4_numbers = rep->targetDuration * CLOCK_FREQ / partduration;
                        }
                    }
                    partUriAttr = keytag->getAttributeByName("URI");
                    if(partUriAttr){
                        uri = partUriAttr->quotedString();
                        if(!uri.empty()){
                            std::istringstream iss(uri);
                            std::ostringstream oss;

                            while(!iss.eof())
                            {
                                char c = iss.peek();
                                if(c == '_'){
                                    break;
                                }else{
                                    iss.get();
                                }
                            }
                            while(!iss.eof())
                            {
                                char c = iss.peek();
                                if(c >= '0' && c <= '9') {
                                    oss.put((char)iss.get());
                                }else if(c == '.')
                                {
                                    iss.get();
                                    break;
                                }else {
                                    iss.get();
                                }
                            }

                            std::string part_index_num = oss.str();
                            oss.str("");
                            std::istringstream is(part_index_num);
                            is.imbue(std::locale("C"));
                            sequenceNumber = 0;
                            is >> sequenceNumber;
                        }
                    }
                    partIndependentAttr = keytag->getAttributeByName("INDEPENDENT");
                    if(partIndependentAttr){
                        partIndependent = true;
                    }
                    keytag = nullptr;
                    if (p_obj && 0)
                        msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Find PART-DURATION(%lld)[%lld],URI[%s],INDEP[%d].", __FILE__ , __FUNCTION__, __LINE__, sequenceNumber, partduration, uri.c_str(), partIndependent);
                    
                    if(uri.empty()){
                        break;
                    }
                    HLSSegment *segment = new (std::nothrow) HLSSegment(rep, sequenceNumber++);
                    if(!segment)
                        break;

                    segment->setSourceUrl(uri);
                    segment->duration.Set(timescale.ToScaled(partduration));
                    segment->startTime.Set(timescale.ToScaled(nzStartTime));
                    segment->independent = partIndependent;
                    nzStartTime += partduration;
                    totalduration += partduration;
                    if(absReferenceTime > VLC_TS_INVALID)
                    {
                        segment->setDisplayTime(absReferenceTime);
                        absReferenceTime += partduration;
                    }

                    segmentstoappend.push_back(segment);

                    if(discontinuity)
                    {
                        segment->discontinuity = true;
                        discontinuity = false;
                    }
                }
            }
            break;

            case SingleValueTag::URI:
            {
                const SingleValueTag *uritag = static_cast<const SingleValueTag *>(tag);
                if(uritag->getValue().value.empty() || partTarget > 0)
                {
                    ctx_extinf = nullptr;
                    ctx_byterange = nullptr;
                    break;
                }

                HLSSegment *segment = new (std::nothrow) HLSSegment(rep, sequenceNumber++);
                if(!segment)
                    break;
                if (p_obj && 0)
                    msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Find new segment[%s].", __FILE__ , __FUNCTION__, __LINE__, uritag->getValue().value.c_str());

                segment->setSourceUrl(uritag->getValue().value);

                /* Need to use EXTXTARGETDURATION as default as some can't properly set segment one */
                mtime_t nzDuration = CLOCK_FREQ * rep->targetDuration;
                if(ctx_extinf)
                {
                    const Attribute *durAttribute = ctx_extinf->getAttributeByName("DURATION");
                    if(durAttribute)
                        nzDuration = CLOCK_FREQ * durAttribute->floatingPoint();
                    ctx_extinf = nullptr;
                }
                segment->duration.Set(timescale.ToScaled(nzDuration));
                segment->startTime.Set(timescale.ToScaled(nzStartTime));
                nzStartTime += nzDuration;
                totalduration += nzDuration;
                if(absReferenceTime > VLC_TS_INVALID)
                {
                    segment->setDisplayTime(absReferenceTime);
                    absReferenceTime += nzDuration;
                }

                segmentstoappend.push_back(segment);

                if(ctx_byterange)
                {
                    std::pair<std::size_t,std::size_t> range = ctx_byterange->getValue().getByteRange();
                    if(range.first == 0) /* first == size, second = offset */
                        range.first = prevbyterangeoffset;
                    prevbyterangeoffset = range.first + range.second;
                    segment->setByteRange(range.first, prevbyterangeoffset - 1);
                    ctx_byterange = nullptr;
                }

                if(discontinuity)
                {
                    segment->discontinuity = true;
                    discontinuity = false;
                }

                if(encryption.method != CommonEncryption::Method::None)
                    segment->setEncryption(encryption);
            }
            break;

            case SingleValueTag::EXTXTARGETDURATION:
                rep->targetDuration = static_cast<const SingleValueTag *>(tag)->getValue().decimal();
                break;

            case SingleValueTag::EXTXPLAYLISTTYPE:
                rep->b_live = (static_cast<const SingleValueTag *>(tag)->getValue().value != "VOD");
                break;

            case SingleValueTag::EXTXBYTERANGE:
                ctx_byterange = static_cast<const SingleValueTag *>(tag);
                break;

            case SingleValueTag::EXTXPROGRAMDATETIME:
                //break;
                rep->b_consistent = false;
                absReferenceTime = VLC_TS_0 +
                        UTCTime(static_cast<const SingleValueTag *>(tag)->getValue().value).mtime();
                /* Reverse apply UTC timespec from first discont */
                if(segmentstoappend.size() && segmentstoappend.back()->getDisplayTime() == VLC_TS_INVALID)
                {
                    mtime_t tempTime = absReferenceTime;
                    for(auto it = segmentstoappend.crbegin(); it != segmentstoappend.crend(); ++it)
                    {
                        mtime_t duration = timescale.ToTime((*it)->duration.Get());
                        if( duration < tempTime - VLC_TS_0 )
                            tempTime -= duration;
                        else
                            tempTime = VLC_TS_0;
                        (*it)->setDisplayTime(tempTime);
                    }
                }
                break;

            case AttributesTag::EXTXKEY:
                parseEncryption(static_cast<const AttributesTag *>(tag),
                                rep->getPlaylistUrl(), encryption);
            break;

            case AttributesTag::EXTXMAP:
            {
                const AttributesTag *keytag = static_cast<const AttributesTag *>(tag);
                const Attribute *uriAttr;
                if(keytag && (uriAttr = keytag->getAttributeByName("URI")) &&
                   !segmentList->initialisationSegment.Get()) /* FIXME: handle discontinuities */
                {
                    InitSegment *initSegment = new (std::nothrow) InitSegment(rep);
                    if(initSegment)
                    {
                        initSegment->setSourceUrl(uriAttr->quotedString());
                        const Attribute *byterangeAttr = keytag->getAttributeByName("BYTERANGE");
                        if(byterangeAttr)
                        {
                            const std::pair<std::size_t,std::size_t> range = byterangeAttr->unescapeQuotes().getByteRange();
                            initSegment->setByteRange(range.first, range.first + range.second - 1);
                        }
                        segmentList->initialisationSegment.Set(initSegment);
                    }
                }
            }
            break;

            case Tag::EXTXDISCONTINUITY:
                discontinuity  = true;
                break;

            case Tag::EXTXENDLIST:
                rep->b_live = false;
                break;
        }
    }

    if (p_obj && 0)
        msg_Dbg(p_obj, "[%s:%s:%d]=zspace=: Find partmp4_numbers=[%d].", __FILE__ , __FUNCTION__, __LINE__, partmp4_numbers);
    if (first && partTarget <= 0) {//HLS start
        int i = segmentstoappend.size();
        for(HLSSegment *seg : segmentstoappend) {
            i--;
            if(i <= 1)
                segmentList->addSegment(seg);
        }
    }else if (first && partTarget > 0) {//LLHLS start
        int i = segmentstoappend.size();
        bool need_play = false;
        bool need_update_num = true;
        int added_num =0;
        for(HLSSegment *seg : segmentstoappend) {
            i--;
            if(i < partmp4_numbers && seg->independent)
                need_play = true;
            if (need_play && ++added_num <= 3) {
                segmentList->addSegment(seg);
                if(need_update_num) {
                    rep->setPlayedSeqNumber(seg->sequence);
                    need_update_num = false;
                }
            }
        }
    }else if (partTarget > 0) {//LLHLS
        uint64_t parsed_num = rep->getPlayedSeqNumber() + 2;
        bool need_update_num = true;
        for(HLSSegment *seg : segmentstoappend) {
            if (seg->sequence >= parsed_num) {
                segmentList->addSegment(seg);
                if(need_update_num) {
                    rep->setPlayedSeqNumber(seg->sequence);
                    need_update_num = false;
                }
            }
        }
    }else {//HLS
        for(HLSSegment *seg : segmentstoappend)
            segmentList->addSegment(seg);
    }
    segmentstoappend.clear();

    if(rep->isLive())
    {
        rep->getPlaylist()->duration.Set(0);
    }
    else if(totalduration > rep->getPlaylist()->duration.Get())
    {
        rep->getPlaylist()->duration.Set(totalduration);
    }

    rep->updateSegmentList(segmentList, true);
}

M3U8 * M3U8Parser::parse(vlc_object_t *p_object, stream_t *p_stream, const std::string &playlisturl)
{
    char *psz_line = vlc_stream_ReadLine(p_stream);
    if(!psz_line || strncmp(psz_line, "#EXTM3U", 7) ||
       (psz_line[7] && !std::isspace(psz_line[7])))
    {
        free(psz_line);
        return nullptr;
    }
    free(psz_line);

    M3U8 *playlist = new (std::nothrow) M3U8(p_object);
    if(!playlist)
        return nullptr;

    if(!playlisturl.empty())
        playlist->setPlaylistUrl( Helper::getDirectoryPath(playlisturl).append("/") );

    BasePeriod *period = new (std::nothrow) BasePeriod( playlist );
    if(!period)
        return playlist;

    std::list<Tag *> tagslist = parseEntries(p_stream);
    bool b_masterplaylist = !getTagsFromList(tagslist, AttributesTag::EXTXSTREAMINF).empty();
    if(b_masterplaylist)
    {
        std::list<Tag *>::const_iterator it;
        std::map<std::string, AttributesTag *> groupsmap;

        /* Preload Session Key */
        Tag *sessionKey = getTagFromList(tagslist, AttributesTag::EXTXSESSIONKEY);
        if(sessionKey)
        {
            CommonEncryption sessionEncryption;
            if(parseEncryption(static_cast<const AttributesTag *>(sessionKey),
                                playlist->getUrlSegment(), sessionEncryption) &&
               !sessionEncryption.uri.empty())
            {
                resources->getKeyring()->getKey(resources, sessionEncryption.uri);
            }
        }

        /* We'll need to create an adaptation set for each media group / alternative rendering
         * we create a list of playlist being and alternative/group */
        std::list<Tag *> mediainfotags = getTagsFromList(tagslist, AttributesTag::EXTXMEDIA);
        for(it = mediainfotags.begin(); it != mediainfotags.end(); ++it)
        {
            AttributesTag *tag = dynamic_cast<AttributesTag *>(*it);
            if(tag && tag->getAttributeByName("URI"))
            {
                std::pair<std::string, AttributesTag *> pair(tag->getAttributeByName("URI")->quotedString(), tag);
                groupsmap.insert(pair);
            }
        }

        /* Then we parse all playlists uri and add them, except when alternative */
        BaseAdaptationSet *adaptSet = new (std::nothrow) BaseAdaptationSet(period);
        if(adaptSet)
        {
            /* adaptSet->setSegmentAligned(true); FIXME: based on streamformat */
            std::list<Tag *> streaminfotags = getTagsFromList(tagslist, AttributesTag::EXTXSTREAMINF);
            for(it = streaminfotags.begin(); it != streaminfotags.end(); ++it)
            {
                AttributesTag *tag = dynamic_cast<AttributesTag *>(*it);
                if(tag && tag->getAttributeByName("URI"))
                {
                    if(groupsmap.find(tag->getAttributeByName("URI")->value) == groupsmap.end())
                    {
                        /* not a group, belong to default adaptation set */
                        HLSRepresentation *rep  = createRepresentation(adaptSet, tag, p_object);
                        if(rep)
                        {
                            adaptSet->addRepresentation(rep);
                        }
                    }
                }
            }
            if(!adaptSet->getRepresentations().empty())
                period->addAdaptationSet(adaptSet);
            else
                delete adaptSet;
        }

        /* Finally add all groups */
        unsigned set_id = 1;
        std::map<std::string, AttributesTag *>::const_iterator groupsit;
        for(groupsit = groupsmap.begin(); groupsit != groupsmap.end(); ++groupsit)
        {
            std::pair<std::string, AttributesTag *> pair = *groupsit;
            if(!pair.second->getAttributeByName("TYPE"))
                continue;

            BaseAdaptationSet *altAdaptSet = new (std::nothrow) BaseAdaptationSet(period);
            if(altAdaptSet)
            {
                HLSRepresentation *rep  = createRepresentation(altAdaptSet, pair.second, p_object);
                if(rep)
                {
                    altAdaptSet->addRepresentation(rep);
                }

                std::string desc;
                if(pair.second->getAttributeByName("GROUP-ID"))
                    desc = pair.second->getAttributeByName("GROUP-ID")->quotedString();
                if(pair.second->getAttributeByName("NAME"))
                {
                    if(!desc.empty())
                        desc += " ";
                    desc += pair.second->getAttributeByName("NAME")->quotedString();
                }

                if(pair.second->getAttributeByName("CODECS"))
                    rep->addCodecs(pair.second->getAttributeByName("CODECS")->quotedString());

                if(!desc.empty())
                {
                    altAdaptSet->description.Set(desc);
                    altAdaptSet->setID(ID(desc));
                }
                else altAdaptSet->setID(ID(set_id++));

                if(pair.second->getAttributeByName("DEFAULT"))
                {
                    if(pair.second->getAttributeByName("DEFAULT")->value == "YES")
                        altAdaptSet->setRole(Role(Role::Value::Main));
                    else
                        altAdaptSet->setRole(Role(Role::Value::Alternate));
                }

                if(pair.second->getAttributeByName("AUTOSELECT"))
                {
                    if(pair.second->getAttributeByName("AUTOSELECT")->value == "NO" &&
                       !pair.second->getAttributeByName("DEFAULT"))
                        altAdaptSet->setRole(Role(Role::Value::Supplementary));
                }

                /* Subtitles unsupported for now */
                const Attribute *typeattr = pair.second->getAttributeByName("TYPE");
                if(typeattr->value == "SUBTITLES")
                {
                    altAdaptSet->setRole(Role(Role::Value::Subtitle));
                }
                else if(typeattr->value != "AUDIO" && typeattr->value != "VIDEO")
                {
                    rep->streamFormat = StreamFormat(StreamFormat::Type::Unsupported);
                }

                if(pair.second->getAttributeByName("LANGUAGE"))
                    altAdaptSet->setLang(pair.second->getAttributeByName("LANGUAGE")->quotedString());

                if(!altAdaptSet->getRepresentations().empty())
                    period->addAdaptationSet(altAdaptSet);
                else
                    delete altAdaptSet;
            }
        }

    }
    else /* Non master playlist (opened directly subplaylist or HLS v1) */
    {
        msg_Dbg(p_object, "[%s:%s:%d]=zspace=: Non master playlist (opened directly subplaylist or HLS v1).", __FILE__ , __FUNCTION__, __LINE__);
        BaseAdaptationSet *adaptSet = new (std::nothrow) BaseAdaptationSet(period);
        if(adaptSet)
        {
            AttributesTag *tag = new AttributesTag(AttributesTag::EXTXSTREAMINF, "");
            tag->addAttribute(new Attribute("URI", playlisturl));
            createAndFillRepresentation(p_object, adaptSet, tag, tagslist);
            if(!adaptSet->getRepresentations().empty())
            {
                adaptSet->getRepresentations().front()->
                    scheduleNextUpdate(std::numeric_limits<uint64_t>::max(), true);
                period->addAdaptationSet(adaptSet);
            }
            else
                delete adaptSet;
            delete tag;
        }
    }

    playlist->addPeriod(period);

    auto xstart = std::find_if(tagslist.cbegin(), tagslist.cend(),
                               [](const Tag *t) {return t->getType() == AttributesTag::EXTXSTART;});
    if(xstart != tagslist.end())
    {
        auto xstartTag = static_cast<const AttributesTag *>(*xstart);
        if(xstartTag->getAttributeByName("TIME-OFFSET"))
        {
            float offset = xstartTag->getAttributeByName("TIME-OFFSET")->floatingPoint();
            if(offset > 0 && (offset * CLOCK_FREQ) <= playlist->duration.Get())
                playlist->presentationStartOffset.Set(CLOCK_FREQ * offset);
            else if(offset < 0 && (-offset * CLOCK_FREQ) <= playlist->duration.Get())
                playlist->presentationStartOffset.Set(playlist->duration.Get() +
                                                      CLOCK_FREQ * offset);
        }
    }

    releaseTagsList(tagslist);

    playlist->debug();
    return playlist;
}

std::list<Tag *> M3U8Parser::parseEntries(stream_t *stream)
{
    std::list<Tag *> entrieslist;
    Tag *lastTag = nullptr;
    bool b_meet_gap = false;
    char *psz_line;

    while((psz_line = vlc_stream_ReadLine(stream)))
    {
        //msg_Dbg(stream, "[%s:%s:%d]=zspace=: [%s].", __FILE__ , __FUNCTION__, __LINE__, psz_line);
        if(*psz_line == '#')
        {
            if(!strncmp(psz_line, "#EXT", 4)) //tag
            {
                std::string key;
                std::string attributes;
                const char *split = strchr(psz_line, ':');
                if(split)
                {
                    key = std::string(psz_line + 1, split - psz_line - 1);
                    attributes = std::string(split + 1);
                }
                else
                {
                    key = std::string(psz_line + 1);
                }

                if(!key.empty())
                {
                    if (!strncmp(key.c_str(), "EXT-X-GAP", 9)) {
                        b_meet_gap = true;
                        //msg_Dbg(stream, "[%s:%s:%d]=zspace=: Meet gap.", __FILE__ , __FUNCTION__, __LINE__);
                    }else if (b_meet_gap) {
                        lastTag = nullptr;
                        //msg_Dbg(stream, "[%s:%s:%d]=zspace=: Meet gap, do not care EXTINF.", __FILE__ , __FUNCTION__, __LINE__);
                    }else {
                        Tag *tag = TagFactory::createTagByName(key, attributes);
                        if(tag)
                            entrieslist.push_back(tag);
                        lastTag = tag;
                        //msg_Dbg(stream, "[%s:%s:%d]=zspace=: Add [%s]=[%s] to entrieslist.", __FILE__ , __FUNCTION__, __LINE__, key.c_str(), attributes.c_str());
                    }
                }
            }
        }
        else if(*psz_line && b_meet_gap == false)
        {
            /* URI */
            if(lastTag && lastTag->getType() == AttributesTag::EXTXSTREAMINF)
            {
                AttributesTag *streaminftag = static_cast<AttributesTag *>(lastTag);
                /* master playlist uri, merge as attribute */
                Attribute *uriAttr = new (std::nothrow) Attribute("URI", std::string(psz_line));
                if(uriAttr)
                    streaminftag->addAttribute(uriAttr);
            }
            else /* playlist tag, will take modifiers */
            {
                Tag *tag = TagFactory::createTagByName("", std::string(psz_line));
                if(tag)
                    entrieslist.push_back(tag);
                //msg_Dbg(stream, "[%s:%s:%d]=zspace=: Add [URI]=[%s] to entrieslist.", __FILE__ , __FUNCTION__, __LINE__, psz_line);
            }
            lastTag = nullptr;
        }
        else // drop
        {
            lastTag = nullptr;
            b_meet_gap = false;
            //msg_Dbg(stream, "[%s:%s:%d]=zspace=: Meet gap, set false.", __FILE__ , __FUNCTION__, __LINE__);
        }

        free(psz_line);
    }

    return entrieslist;
}
