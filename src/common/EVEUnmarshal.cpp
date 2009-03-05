/*
	------------------------------------------------------------------------------------
	LICENSE:
	------------------------------------------------------------------------------------
	This file is part of EVEmu: EVE Online Server Emulator
	Copyright 2006 - 2008 The EVEmu Team
	For the latest information visit http://evemu.mmoforge.org
	------------------------------------------------------------------------------------
	This program is free software; you can redistribute it and/or modify it under
	the terms of the GNU Lesser General Public License as published by the Free Software
	Foundation; either version 2 of the License, or (at your option) any later
	version.

	This program is distributed in the hope that it will be useful, but WITHOUT
	ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
	FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License along with
	this program; if not, write to the Free Software Foundation, Inc., 59 Temple
	Place - Suite 330, Boston, MA 02111-1307, USA, or go to
	http://www.gnu.org/copyleft/lesser.txt.
	------------------------------------------------------------------------------------
	Author:		BloodyRabbit, Captnoord, Zhur
*/

#include "common.h"
#include "PyRep.h"
#include "logsys.h"
#include "../common/packet_dump.h"
#include "../common/packet_functions.h"
#include "../common/misc.h"
#include "EVEMarshalOpcodes.h"
#include "EVEZeroCompress.h"
#include "MarshalReferenceMap.h"
#include <stdio.h>
#include <string>

using std::string;

#ifndef WIN32
#define INT_MAX 0x7FFFFFFF
#endif

class UnmarshalState {
public:
	UnmarshalState(uint8 save_count_in)
	: count_packedobj(0),
	  save_count(save_count_in) 
	{
	}

	uint32 count_packedobj;
	
	const uint8 save_count;
	vector<PyRep *> saves;	//we do not own the pointers in this list
};

static uint32 UnmarshalData(UnmarshalReferenceMap *state, const uint8 *packet, uint32 len, PyRep *&res, const char *indent);

//returns ownership
PyRep *InflateAndUnmarshal(const uint8 *body, uint32 body_len)
{
	const uint8 *const orig_body = body;
	const uint32 orig_body_len = body_len;
	
	bool   inflated = false;
	uint8* work_buffer = *((uint8**)&body);
	uint8* inflated_buffer = NULL;
	
	if(*body != SubStreamHeaderByte) // peek first character
	{
		if(body_len > sizeof(uint32) && *((const uint32 *) body) == 0)
		{
			//winging it here...
			body_len -= 12;
			inflated_buffer = InflatePacket(body+12, &body_len);
			if(inflated_buffer == NULL)
			{
				_log(NET__PRES_ERROR, "Failed to inflate special packet!");
				_log(NET__PRES_DEBUG, "Raw Hex Dump:");
				_hex(NET__PRES_DEBUG, inflated_buffer, body_len);
				return NULL;
			//} else {
			//	_log(NET__UNMARSHAL_ERROR, "Special Inflated packet of len %d to length %d\n", orig_body_len, body_len);
			}
			inflated = true;
		}
		else
		{
			inflated_buffer = InflatePacket(body, &body_len);
			if(inflated_buffer == NULL)
			{
				_log(NET__PRES_ERROR, "Failed to inflate packet!");
				_log(NET__PRES_DEBUG, "Raw Hex Dump:");
				_hex(NET__PRES_DEBUG, inflated_buffer, body_len);
				return NULL;
			//} else {
			//	body = buf;
			//	_log(NET__PRES_TRACE, "Inflated packet of len %d to length %d", orig_body_len, body_len);
			}
			inflated = true;
		}
	} //end inflation conditional

	const uint8 *post_inflate_body = inflated_buffer;

	/* if we have inflated data replace our work buffer*/
	if (inflated_buffer != NULL)
		work_buffer = inflated_buffer;
	
	//_log(NET__PRES_RAW, "Raw Hex Dump (post-inflation):");
	//phex(NET__PRES_RAW, work_buffer, body_len);

	//skip SubStreamHeaderByte
	work_buffer++;
	body_len--;

	uint8 save_count = *work_buffer;
	//no idea what the other three bytes are, this might be a uint32, but that would be stupid
	work_buffer += 4;
	body_len -= 4;

	UnmarshalReferenceMap state(save_count);
	
	PyRep *rep;
	uint32 used_len = UnmarshalData(&state, work_buffer, body_len, rep, "    ");
	if(rep == NULL) {
		_log(NET__PRES_ERROR, "Failed to unmarshal data!");
		if(post_inflate_body != orig_body)
			delete[] post_inflate_body;
		return NULL;
	}

	//total shit:
    if(state.GetStoredCount() > 0) {
		uint32 index = 0;
		uint32 rlen = body_len-used_len;
		for(index = 0; index < state.GetStoredCount(); index++) {
			if(rlen < sizeof(uint32)) {
				_log(NET__UNMARSHAL_TRACE, "Insufficient length for hack 0x2d trailer %d/%d", index+1, state.GetStoredCount());
			} else {
				uint32 val = *((const uint32 *) (work_buffer+used_len));
				
				_log(NET__UNMARSHAL_TRACE, "Hack 0x23/Obj2 value[%d]: 0x%lx", index, val);
				
				used_len += sizeof(uint32);
				rlen -= sizeof(uint32);
			}
		}
    }

	if(used_len != body_len) {
		_log(NET__UNMARSHAL_TRACE, "Unmarshal did not consume entire data: %d/%d used", used_len, body_len);
		_hex(NET__UNMARSHAL_TRACE, work_buffer+used_len, body_len-used_len);
	}

	if (inflated == true)
		SafeDeleteArray(inflated_buffer);
	
	return(rep);
}

static uint32 UnmarshalData(UnmarshalReferenceMap *state, const uint8 *packet, uint32 len, PyRep *&res, const char *pfx) {
/************************************************************************/
/*                                                                      */
/************************************************************************/

#define GetByType(x) \
	*(x*)packet;\
	packet+= sizeof(x);\
	len -= sizeof(x);\
	len_used += sizeof(x);

#define IncreaseIndex(count) \
	packet+= count;\
	len -= count;\
	len_used += count;

#define Getuint8() GetByType(uint8)
#define Getchar() GetByType(char)
#define Getuint16() GetByType(uint16)
#define Getuint32() GetByType(uint32)
#define Getuint64() GetByType(uint64)
#define GetDouble() GetByType(double)

/************************************************************************/
/*                                                                      */
/************************************************************************/

	res = NULL;
	uint32 len_used = 0;
	if(len < 1) {
		_log(NET__PRES_ERROR, "Empty packet received by UnmarshalData\n");
		return(len_used);
	}
	
	uint8 raw_opcode = Getuint8();
	
	if(raw_opcode & PyRepSaveMask) {
		_log(NET__UNMARSHAL_TRACE, "Raw opcode 0x%x has PyRepSaveMask set", raw_opcode);
	}
	if(raw_opcode & PyRepUnknownMask) {
		_log(NET__UNMARSHAL_TRACE, "Raw opcode 0x%x has PyRepUnknownMask set", raw_opcode);
	}
	
	uint8 opcode = raw_opcode & PyRepOpcodeMask;
	
	_log(NET__UNMARSHAL_TRACE, "%sOpcode 0x%02x", pfx, opcode);
	
	switch(opcode)
	{
		case Op_PyNone:
		{
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyNone", pfx, opcode);
			res = new PyRepNone();
		}break;
	
	case Op_PyByteString:
	{
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for string length (missing length and data)\n");
			break;
		}
		uint8 str_len = Getuint8();
	
		if(len < str_len) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of data in string of length %d, %d bytes remain.\n", str_len, len);
			break;
		}

		PyRepString *r = new PyRepString(packet, str_len, true);
		
		if(ContainsNonPrintables(r->value.c_str())) {
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyByteString(len=%d, <binary>)", pfx, opcode, str_len);
			_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
			phex(NET__UNMARSHAL_BUFHEX, packet, str_len);
		} else
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyByteString(len=%d, \"%s\")", pfx, opcode, str_len, r->value.c_str());

		res = r;
		
		len_used += str_len;
		packet += str_len;
		len -= str_len;
		
		break; }
	
	case Op_PyLongLong: {
		if(len < 8) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for long long argument\n");
			break;
		}
		uint64 data = *((const uint64 *) packet);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyLongLong %llu", pfx, opcode, data);
		
		res = new PyRepInteger(data);
		
		packet += sizeof(uint64);
		len -= sizeof(uint64);
		len_used += sizeof(uint64);
		
		break; }
	
	case Op_PyLong: {
	
		if(len < sizeof(uint32)) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for long arg\n");
			break;
		}
		uint32 value = *((const uint32 *) packet);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyLong %lu", pfx, opcode, value);
		
		res = new PyRepInteger(value);
		
		packet += sizeof(value);
		len -= sizeof(value);
		len_used += sizeof(value);
		
		break; }
	
	case Op_PySignedShort: {
	
		if(len < sizeof(uint16)) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for short arg\n");
			break;
		}
		int16 value = *((const int16 *) packet);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyShort %d", pfx, opcode, value);
		
		res = new PyRepInteger(value);
		
		packet += sizeof(value);
		len -= sizeof(value);
		len_used += sizeof(value);
		
		break; }
	
	case Op_PyByte: {
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for byte integer arg\n");
			break;
		}
		uint8 value = *packet;
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyByte %u", pfx, opcode, value);
		
		res = new PyRepInteger(value);
		
		packet++;
		len--;
		len_used++;
		
		break; }
	
	case Op_PyMinusOne:
		{ 
			//.text:1005D11D loc_1005D11D:
			//this looks like it is a 0-length opcode based on the asm.
			// 
			// might want to check out the agentMgr.GetAgents cached object for this one...
			
			//TODO: I think this can be used for floats and ints... further, I think
			//we need a better internal representation of this...
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyMinusOne", pfx, opcode);
			res = new PyRepInteger(INT_MAX);
		break; }
	
	case Op_PyZeroInteger: {
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyZeroInteger", pfx, opcode);
		res = new PyRepInteger(0);
		break; }
	
	case Op_PyOneInteger: {
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyOneInteger", pfx, opcode);
		res = new PyRepInteger(1);
		break; }
	
	case Op_PyReal: {
	
		if(len < sizeof(double)) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for double arg\n");
			break;
		}
		double value = *((const double *) packet);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyReal %.13f", pfx, opcode, value);
	
		res = new PyRepReal(value);
		
		packet += sizeof(value);
		len -= sizeof(value);
		len_used += sizeof(value);
		
		break; }
	
	case Op_PyZeroReal:
		{
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyZeroReal", pfx, opcode);
			res = new PyRepReal(0.0);
		break; }

	//0xC?
	
	case Op_PyBuffer: {
		
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for data buffer's length byte\n");
			break;
		}
		uint32 data_length = *packet;
		packet++;
		len--;
		len_used++;
	
		if(data_length == 0xFF) {
			//extended length data buffer
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for data buffer length 4-byte element\n");
				break;
			}
			data_length = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
	
		if(len < data_length) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for data buffer's data, %d needed but %d remain\n", data_length, len);
			break;
		}
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyBuffer length %d", pfx, opcode, data_length);
		_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
		phex(NET__UNMARSHAL_BUFHEX, packet, data_length);
		
		res = new PyRepBuffer(packet, data_length);
		
		packet += data_length;
		len -= data_length;
		len_used += data_length;
		
		break; }
	
	case Op_PyEmptyString: {
		//.text:1005D1EF loc_1005D1EF
		// 
		//not 100% sure about this one.
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyEmptyString", pfx, opcode);
		res = new PyRepString("");
		break; }
	
	case Op_PyCharString: {
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for byte data\n");
			break;
		}
		char value[2];
		value[0] = *packet;
		value[1] = '\0';
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyCharString %c", pfx, opcode, value[0]);

		res = new PyRepString(value);
		
		packet++;
		len--;
		len_used++;
		
		break; }
	
	case Op_PyByteString2: {	//SHORT_BINSTRING
		
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for string length (missing length and data)\n");
			break;
		}
		uint32 str_len = *packet;
		packet++;
		len--;
		len_used++;
				
		if(len < str_len) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of data in string of length %d, %d bytes remain.\n", str_len, len);
			break;
		}
		
		PyRepString *r = new PyRepString(packet, str_len, false);
		res = r;

		if(ContainsNonPrintables(r->value.c_str())) {
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyByteString2(len=%d, <binary>)", pfx, opcode, str_len);
			_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
			phex(NET__UNMARSHAL_BUFHEX, packet, str_len);
		} else
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyByteString2(len=%d, \"%s\")", pfx, opcode, str_len, r->value.c_str());
		
		packet += str_len;
		len -= str_len;
		len_used += str_len;
		
		break; }
	
	case Op_PyStringTableItem: {	//an item from the string table
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for String Table Item argument\n");
			break;
		}
		uint8 value = *packet;

		const char *v = PyRepString::GetStringTable()->LookupIndex(value);
		if(v == NULL) {
			_log(NET__UNMARSHAL_ERROR, "String Table Item %d is out of range!\n", value);
			char ebuf[64];
			snprintf(ebuf, 64, "Invalid String Table Item %d", value);
			res = new PyRepString(ebuf);
		} else {
			res = new PyRepString(v);
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyStringTableItem(%d) = %s", pfx, opcode, value, v);
		}
		
		packet++;
		len--;
		len_used++;
		
		break; }
	
	case Op_PyUnicodeByteString: {
		if(len < 1)
		{
			_log(NET__UNMARSHAL_ERROR, "Not enough data for wchar_t string argument\n");
			break;
		}

		uint32 stringLength = Getuint8();

		if (stringLength == 0xFF)
		{
			if (len < 4)
			{
				//_log(NET__UNMARSHAL_ERROR, "Not enough data for wchar_t string argument\n");
				break;
			}
			stringLength = Getuint32();
		}
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyUnicodeByteString of len %d*2", pfx, opcode, stringLength);
		_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
		phex(NET__UNMARSHAL_BUFHEX, packet, stringLength * 2);
		
		res = new PyRepString(packet, stringLength * 2, false);
		
		IncreaseIndex(stringLength*2);
		break; }
	
	case 0x13:
		{ _log(NET__UNMARSHAL_ERROR, "Unhandled field type 0x%x\n", opcode);
			phex(NET__UNMARSHAL_ERROR, packet, len>32?32:len);
		break; }

	/*                                                                              
    *
    *   Variable length tuple
    *
    */
	case Op_PyTuple: {
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for tuple length (missing length and data)\n");
			break;
		}
		uint32 data_len = *packet;
		packet++;
		len--;
		len_used++;
		
		if(data_len == 0xFF) {
			//extended length data buffer
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for tuple length 4-byte element\n");
				break;
			}
			data_len = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}

		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyTuple(%d)", pfx, opcode, data_len);
		
		PyRepTuple *tuple = new PyRepTuple(data_len);
		res = tuple;

		char t[15];
		
		uint32 r;
		for(r = 0; r < data_len; r++) {
			std::string n(pfx);
			snprintf(t, 14, "  [%2ld] ", r);
			n += t;
			uint32 field_len = UnmarshalData(state, packet, len, tuple->items[r], n.c_str());
			if(tuple->items[r] == NULL) {
				delete tuple;
				res = NULL;
				break;
			}
			packet += field_len;
			len -= field_len;
			len_used += field_len;
		}
		
		break; }
	
	/*                                                                              
    *
    *   Variable length list
    *
    */
	case Op_PyList: {
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for list length (missing length and data)\n");
			break;
		}
		uint32 data_len = *packet;
		packet++;
		len--;
		len_used++;
		
		if(data_len == 0xFF) {
			//extended length data buffer
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for list length 4-byte element\n");
				break;
			}
			data_len = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyList(%d)", pfx, opcode, data_len);
		
		PyRepList *rep = new PyRepList();
		res = rep;
	
		char t[15];
		uint32 r;
		for(r = 0; r < data_len; r++) {
			std::string n(pfx);
			snprintf(t, 14, "  [%2ld] ", r);
			n += t;
			PyRep *rep2;
			uint32 field_len = UnmarshalData(state, packet, len, rep2, n.c_str());
			if(rep2 == NULL) {
				delete rep;
				res = NULL;
				break;
			}
			rep->items.push_back(rep2);
			packet += field_len;
			len -= field_len;
			len_used += field_len;
		}
		
		break; }
	
	/*                                                                              
    *
    *   Dictionary                                                                 
    *
    */
	case Op_PyDict: {
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for element count in new dict\n");
			break;
		}
		uint32 data_len = *packet;
		packet++;
		len--;
		len_used++;
		
		if(data_len == 0xFF) {
			//extended length data buffer
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for dict length 4-byte element\n");
				break;
			}
			data_len = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyDict(%d)", pfx, opcode, data_len);

		//total hack until we understand this 0x55 thing better
		//this seems to only happen if the first byte adter 0x7e(~) is 1
        /*if(*packet == 0x55) {
			packet++;
			len--;
			len_used++;
			
			if(len < 1) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for hack 0x55\n");
				break;
			}
			byte unknown = *packet;
			packet++;
			len--;
			len_used++;
	
			_log(NET__UNMARSHAL_TRACE, "%s  Detected 0x55 save marker with arg 0x%x", pfx, unknown);

			//not sure how to get this number yet...
			data_len = 0xa;
		}*/
		
		
		PyRepDict *rep = new PyRepDict();
		res = rep;
		
		char t[17];
		uint32 r;
		for(r = 0; r < data_len; r++) {
			PyRep *key;
			PyRep *value;
			
			std::string m(pfx);
			snprintf(t, 16, "  [%2ld] Value: ", r);
			m += t;
			uint32 field_len = UnmarshalData(state, packet, len, value, m.c_str());
			if(value == NULL) {
				delete rep;
				res = NULL;
				break;
			}
			packet += field_len;
			len -= field_len;
			len_used += field_len;
			
			std::string n(pfx);
			snprintf(t, 16, "  [%2ld] Key: ", r);
			n += t;
			field_len = UnmarshalData(state, packet, len, key, n.c_str());
			if(key == NULL) {
				delete rep;
				delete value;
				res = NULL;
				break;
			}
			packet += field_len;
			len -= field_len;
			len_used += field_len;

			rep->items[key] = value;
		}
		break; }
	
	/*                                                                              
    *
    *   "Object".. not sure on this one.
    *
    */
	case Op_PyObject: {
		PyRep *type;
		PyRep *arguments;
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyObject", pfx, opcode);
		
		std::string n(pfx);
		n += "  Type: ";
		
		uint32 type_len = UnmarshalData(state, packet, len, type, n.c_str());
		if(type == NULL)
			break;
		packet += type_len;
		len -= type_len;
		len_used += type_len;

		std::string typestr;
		if(!type->IsString()) {
			typestr = "NON-STRING-TYPE";
		} else {
			PyRepString *s = (PyRepString *) type;
			typestr = s->value;
		}
		delete type;
	
		if(len == 0) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of length in dict entry for key!\n");
			break;
		}

		n = pfx;
		n += "  Args: ";
		uint32 value_len = UnmarshalData(state, packet, len, arguments, n.c_str());
		if(arguments == NULL)
			break;
		packet += value_len;
		len -= value_len;
		len_used += value_len;

		res = new PyRepObject(typestr, arguments);
		break; }
	
	case 0x18:
		{ _log(NET__UNMARSHAL_ERROR, "Unhandled field type 0x%x\n", opcode);
			phex(NET__UNMARSHAL_ERROR, packet, len>32?32:len);
		break; }
	
	case Op_PySubStruct: {

        // This is really a remote object specification
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PySubStruct", pfx, opcode);
		
		std::string n(pfx);
		n += "  ";
	
		PyRep *ss;
		uint32 value_len = UnmarshalData(state, packet, len, ss, n.c_str());
		if(ss == NULL)
			break;
		packet += value_len;
		len -= value_len;
		len_used += value_len;

		res = new PyRepSubStruct(ss);
		
		break; }
	
	case 0x1A:
		{ _log(NET__UNMARSHAL_ERROR, "Unhandled field type 0x%x\n", opcode);
			phex(NET__UNMARSHAL_ERROR, packet, len>32?32:len);
		break; }
	
	case Op_PySavedStreamElement:
		{ 
			if(len < 1)
			{
				_log(NET__UNMARSHAL_ERROR, "Not enough data for saved reference index\n");
				break;
			}
			uint32 reference_index = Getuint8();
			if (reference_index == 0xFF)
			{
				// TODO: add size check..
				reference_index = Getuint32();
			}

			// referenced objects start with 1
			if (reference_index == 0)
			{
				_log(NET__UNMARSHAL_ERROR, "Save index 0 is invalid");
				break;
			}

			if(reference_index > state->GetMaxStoredCount())
			{
				_log(NET__UNMARSHAL_ERROR, "Save index %d is higher than the save count %d for this stream\n", reference_index, state->GetMaxStoredCount());
				break;
			}

			if(reference_index > state->GetStoredCount())
			{
				_log(NET__UNMARSHAL_ERROR, "Save index %d is higher than the number of saved elements %d for this stream so far\n", reference_index, state->GetStoredCount());
				break;
			}

			//_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PySavedStreamElement with index 0x%x", pfx, opcode, reference_index);

			res = state->GetStoredObject(reference_index)->Clone();
		} break;
	
	case Op_PyChecksumedStream: {
		if(len < sizeof(uint32)) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for checksum argument\n");
			break;
		}
		uint32 lval = *((const uint32 *) packet);
		
		packet += sizeof(uint32);
		len -= sizeof(uint32);
		len_used += sizeof(uint32);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyChecksumedStream w/ sum %lu", pfx, opcode, lval);
		
		std::string n(pfx);
		n += "  ";
		
		PyRep *stream;
		uint32 sslen = UnmarshalData(state, packet, len, stream, n.c_str());
		if(stream == NULL)
			break;
		
		packet += sslen;
		len -= sslen;
		len_used += sslen;

		res = new PyRepChecksumedStream(lval, stream);
		
		break; }
	
	case Op_PyTrue: {	//_Py_TrueStruct (which is Py_True)
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyTrue", pfx, opcode);
		res = new PyRepBoolean(true);
		break; }
	
	case Op_PyFalse: {	//_Py_ZeroStruct (which is Py_False)
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyFalse", pfx, opcode);
		res = new PyRepBoolean(false);
		break; }
	
	case 0x21:
		{ _log(NET__UNMARSHAL_ERROR, "Unhandled field type 0x%x\n", opcode);
			phex(NET__UNMARSHAL_ERROR, packet, len>32?32:len);
		break; }
	
	case Op_NewObject1:
	case Op_NewObject2:
		{
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_NewObject", pfx, opcode);

		PyRep *header;

		std::string n(pfx);
		n += "    ";
		uint32 clen = UnmarshalData(state, packet, len, header, n.c_str());
		if(header == NULL)
			break;
		packet += clen;
		len -= clen;
		len_used += clen;

		PyRepNewObject *obj = new PyRepNewObject(opcode == Op_NewObject2, header);

		n = pfx;
		n += "  ListData: ";
		while(*packet != Op_PackedTerminator) {
			PyRep *el;

			clen = UnmarshalData(state, packet, len, el, n.c_str());
			if(el == NULL)
				break;
			packet += clen;
			len -= clen;
			len_used += clen;

			obj->list_data.push_back(el);
		}
		packet++;
		len--;
		len_used++;

		n = pfx;
		n += "  DictData: ";
		while(*packet != Op_PackedTerminator) {
			PyRep *key;
			PyRep *value;

			clen = UnmarshalData(state, packet, len, key, n.c_str());
			if(key == NULL)
				break;
			packet += clen;
			len -= clen;
			len_used += clen;

			clen = UnmarshalData(state, packet, len, value, n.c_str());
			if(value == NULL)
				break;
			packet += clen;
			len -= clen;
			len_used += clen;

			obj->dict_data[key] = value;
		}
		packet++;
		len--;
		len_used++;

		res = obj;
		break;
		}

	case Op_PyEmptyTuple: {
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyEmptyTuple", pfx, opcode);
		res = new PyRepTuple(0);
		break; }
	
	case Op_PyOneTuple: {
		//TUPLE1
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyOneTuple", pfx, opcode);
		std::string n(pfx);
		n += "  [0] ";

		PyRep *i;
		uint32 key_len = UnmarshalData(state, packet, len, i, n.c_str());
		if(i == NULL)
			break;
		packet += key_len;
		len -= key_len;
		len_used += key_len;

		PyRepTuple *tuple = new PyRepTuple(1);
		tuple->items[0] = i;
		res = tuple;
		
		break; }
	
	case Op_PyEmptyList: {
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyEmptyList", pfx, opcode);
		res = new PyRepList();
		break; }

	//single element list.
	case Op_PyOneList: {
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyOneList", pfx, opcode);
		std::string n(pfx);
		n += "  [0] ";
		
		PyRep *i;
		uint32 key_len = UnmarshalData(state, packet, len, i, n.c_str());
		if(i == NULL)
			break;
		packet += key_len;
		len -= key_len;
		len_used += key_len;
	
		PyRepList *l = new PyRepList();
		l->items.push_back(i);
		res = l;
		
		break; }

	//empty unicode string.
	case Op_PyEmptyUnicodeString: {	//'('
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyEmptyUnicodeString", pfx, opcode);
		res = new PyRepString("");
		break; }

	//single wchar_t unicode string
	case Op_PyUnicodeCharString: {	//')'
		if(len < sizeof(uint16)) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for unicode char arg\n");
			break;
		}
		uint16 lval = *((const uint16 *) packet);
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyUnicodeCharString 0x%x", pfx, opcode, lval);
		
		res = new PyRepString((const uint8 *) &lval, sizeof(uint16), false);
		
		packet += sizeof(uint16);
		len -= sizeof(uint16);
		len_used += sizeof(uint16);
		
		break; }
	
	case Op_PyPackedRow:	//*
		{
        /*
         * As far as I can tell, this opcode is really a packed
         * form of blue.DBRow, which takes a row descriptor and a
         * data stream (the work_buffer here)
         *
         */
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyPackedRow", pfx, opcode);
			
#ifdef PACKED_ROW_HEADER_HACK
		/*
         * These first two bytes are really an entire object
         * in the form of an 0x1B (Op_PySavedStreamElement)
         * and its associated index. This index points back to
         * the blue.DBRowDescriptor for this DBRow...
         *
        */
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for packed u1\n");
			break;
		}
		uint8 u1 = *packet;
		packet++;
		len--;
		len_used++;
		if(u1 != Op_PySavedStreamElement) {
			_log(NET__UNMARSHAL_ERROR, "Unexpected u1 opcode (0x%x) in Op_PyPacked. This should have been a saved stream element", u1);
			break;
		}
		
		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for packed type");
			break;
		}
		uint8 header_element = *packet;
		packet++;
		len--;
		len_used++;

        /* end "this should be a recursive unmarshal" hack */
#else
        /* screw efficiency for now, just unmarshal the damned thing. */
		
		std::string n(pfx);
		n += "  Header ";
		PyRep *header_element;
		uint32 header_len = UnmarshalData(state, packet, len, header_element, n.c_str());
		if(header_element == NULL) {
			break;
		}
		packet += header_len;
		len -= header_len;
		len_used += header_len;
#endif
		PyRepPackedRow *row = new PyRepPackedRow(header_element, true);

		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for packed length (missing length and data)\n");
			break;
		}
		uint32 data_len = *packet;
		packet++;
		len--;
		len_used++;

		//assumed, not actually observed:
		if(data_len == 0xFF) {
			//extended length
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for packed length 4-byte element");
				break;
			}
			data_len = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		if(len < data_len) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of data in packed of length %d, %d bytes remain.\n", data_len, len);
			break;
		}
		
		_log(NET__UNMARSHAL_BUFHEX, "%s  Packed Contents: len=%lu", pfx, data_len);
		phex(NET__UNMARSHAL_BUFHEX, packet, data_len);

		//TODO: we can theoretically calculate the unpacked length
        //based on the dbrowdescriptor for this packed data... and
        //we need this information to determine how many zeros to fill
        //the end of the buffer with, but im leaving that to somebody
        //else at a higher layer which understands the dbrowdesc.
		std::vector<uint8> unpacked;
		UnpackZeroCompressed(packet, data_len, unpacked);
		packet += data_len;
		len -= data_len;
		len_used += data_len;
		
		_log(NET__UNMARSHAL_TRACE, "%s  Unpacked Contents: len=%lu", pfx, unpacked.size());
		phex(NET__UNMARSHAL_TRACE, &unpacked[0], unpacked.size());
		
#ifdef PACKED_ROW_HEADER_HACK
		//do something...
#else
		row->Push(&unpacked[0], (uint32)unpacked.size());
#endif
		//TODO: unmarshal following PyReps as well
		res = row;
		break; }
	
	case Op_PySubStream: {	//'+'
	
		if(len < 6) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for substream\n");
			break;
		}
	
		uint32 data_length = *packet;
		packet++;
		len--;
		len_used++;
			
		//special extended length substream
		if(data_length == 0xFF) {
			if(len < 10) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for a long substream\n");
				break;
			}
			
			data_length = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		if(*packet != SubStreamHeaderByte) {
			_log(NET__UNMARSHAL_ERROR, "Invalid header byte on substream data 0x%02x\n", *packet);
			break;
		}
		
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PySubStream of length %lu", pfx, opcode, data_length);

		res = new PyRepSubStream(packet, data_length);
		
		packet += data_length;
		len -= data_length;
		len_used += data_length;
		
		break; }

	//TUPLE2
	case Op_PyTwoTuple: {

		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyTwoTuple", pfx, opcode);
		std::string n(pfx);
		n += "  [0] ";
		
		PyRep *i;
		uint32 key_len = UnmarshalData(state, packet, len, i, n.c_str());
		if(i == NULL)
			break;
		packet += key_len;
		len -= key_len;
		len_used += key_len;

		n = pfx;
		n += "  [1] ";
		PyRep *j;
		uint32 val_len = UnmarshalData(state, packet, len, j, n.c_str());
		if(j == NULL) {
			delete i;
			break;
		}
		packet += val_len;
		len -= val_len;
		len_used += val_len;

		PyRepTuple *tuple = new PyRepTuple(2);
		tuple->items[0] = i;
		tuple->items[1] = j;
		res = tuple;
		
		break; }
	
	case Op_PackedTerminator:
		{

        /*
        _log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_SomeUnkownMarker66 total hack", pfx,opcode);
        std::string n(pfx); n += " 0x2D ";
		
		PyRep *i;
		uint32 e_len = UnmarshalData(state, packet, len, i, n.c_str());
		if(i == NULL)
			break;
		packet += e_len;
		len -= e_len;
		len_used += e_len;*/
		
        /*_log(NET__UNMARSHAL_TRACE, "(0x%x)Op_SomeUnkownMarker2D total hack", opcode);

		state->count_0x2d++;
		
		PyRep *i;
		uint32 key_len = UnmarshalData(state, packet, len, i, pfx);
		if(i == NULL)
			break;
		packet += key_len;
		len -= key_len;
		len_used += key_len;
        res = i;*/

			
		//hacking... should be its own type, but the only other acceptable thing 
		// is an OP_Packed, so this works fine.
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PackedTerminator", pfx,opcode);
		res = new PyRepNone();

		
/* total crap... */
/*		if(len < 1) {
			_log(NET__UNMARSHAL_ERROR, "Not enough data for saved reference index\n");
			break;
		}
		uint8 save_index = *packet;
		packet++;
		len--;
		len_used++;

		_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_SomeUnkownMarker total hack 2", pfx, opcode, save_index+1);
		int r;
		PyRep *i;
		for(r = 0; r < 2; r++) {
			
			std::string n(pfx);
			n += " Key1: ";
			
			uint32 e_len = UnmarshalData(state, packet, len, i, n.c_str());
			if(i == NULL)
				break;
			packet += e_len;
			len -= e_len;
			len_used += e_len;
	
			n = pfx;
			n += " Value1: ";
			PyRep *i2;
			e_len = UnmarshalData(state, packet, len, i2, n.c_str());
			if(i2 == NULL)
				break;
			packet += e_len;
			len -= e_len;
			len_used += e_len;
		}
		res = i;*/
/*vicious hack trying to understand this..
  it seems like this opcode is frequently repeated, not sure what
  it means yet.
		_log(NET__UNMARSHAL_TRACE, "%s(0x%x) UNKNOWN", pfx, opcode);
		std::string n(pfx);
		n += "  -0-> ";
		
		PyRep *i;
		uint32 key_len = UnmarshalData(state, packet, len, i, n.c_str());
		if(i == NULL)
			break;
		packet += key_len;
		len -= key_len;
		len_used += key_len;

		
		n = pfx;
		n += "  -1-> ";
		PyRep *j;
		uint32 val_len = UnmarshalData(state, packet, len, j, n.c_str());
		if(j == NULL) {
			delete i;
			break;
		}
		packet += val_len;
		len -= val_len;
		len_used += val_len;

		
		n = pfx;
		n += "  -2-> ";
		PyRep *k;
		uint32 gg_len = UnmarshalData(state, packet, len, k, n.c_str());
		if(k == NULL) {
			delete i;
			delete j;
			break;
		}
		packet += gg_len;
		len -= gg_len;
		len_used += gg_len;

			
        res = new PyRepNone();
        */
//			_log(NET__UNMARSHAL_ERROR, "Unhandled field type 0x%x (maybe code related)", opcode);
//          _hex(NET__UNMARSHAL_ERROR, packet, len>32?32:len);
		break; }
	
	case Op_PyUnicodeString: {
	
		uint32 data_length = *packet;
		packet++;
		len--;
		len_used++;
			
		//special extended length substream
		if(data_length == 0xFF) {
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for a long unicode string\n");
				break;
			}
			
			data_length = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		if(len < data_length) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of data in BINUNICODE string of length %d, %d bytes remain.\n", data_length, len);
			break;
		}
		len_used += data_length;

		PyRepString *r = new PyRepString(packet, data_length, false);
		
		if(ContainsNonPrintables(r->value.c_str())) {
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyUnicodeString(len=%d, <binary>)", pfx, opcode, data_length);
			_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
			phex(NET__UNMARSHAL_BUFHEX, packet, data_length);
		} else
			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyUnicodeString(len=%d, \"%s\")", pfx, opcode, data_length, r->value.c_str());

		
		res = r;
		
		break; }

	
	case Op_PyVarInteger: {

        /* this is one of the stranger fields I have found, it seems to be a variable
         * length integer field (somewhat of a 'bigint' style data type), but it gets
         * used at times for integers which would fit into the other primitive data
         * types.... I would really like to see the criteria they use to determine
         * what gets marshales as what...
         */

		uint32 data_length = *packet;
		packet++;
		len--;
		len_used++;
			
		//special extended length (I assume this wont happen for integers..., at least I hope not..)
		if(data_length == 0xFF) {
			if(len < sizeof(uint32)) {
				_log(NET__UNMARSHAL_ERROR, "Not enough data for an 0x2F\n");
				break;
			}
			
			data_length = *((const uint32 *) packet);
			packet += sizeof(uint32);
			len -= sizeof(uint32);
			len_used += sizeof(uint32);
		}
		
		if(len < data_length) {
			_log(NET__UNMARSHAL_ERROR, "Ran out of data in 0x2F of length %d, %d bytes remain.\n", data_length, len);
			break;
		}
		len_used += data_length;

		if(data_length <= sizeof(uint64)) {
			uint64 intval = (1LL << (8 * data_length)) - 1;
			intval &= *((const uint64 *) packet);

			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyVarInteger(len=%d) = " I64u, pfx, opcode, data_length, intval);
			res = new PyRepInteger(intval);
		} else {
			//uint64 is not big enough
			//just pass it up to the application layer as a buffer...
			PyRepBuffer *r = new PyRepBuffer(packet, data_length);

			_log(NET__UNMARSHAL_TRACE, "%s(0x%x)Op_PyVarInteger(len=%d)", pfx, opcode, data_length);
			_log(NET__UNMARSHAL_BUFHEX, "%s  Buffer Contents:", pfx);
			_hex(NET__UNMARSHAL_BUFHEX, packet, data_length);
			
			res = r;
		}
		
		break; }
	
	default:
		{ _log(NET__UNMARSHAL_ERROR, "Unhandled (default) field type 0x%x\n", opcode);
			phex(NET__UNMARSHAL_ERROR, packet-1, len>32?32:len);
		} break;
		}	//end switch
	if(raw_opcode & PyRepSaveMask) {
		//save off this pointer. We do not need to clone it since this function is the only
		//thing with a pointer to it, and we know it will remain valid until we are done.
		state->StoreReferencedObject(res);
	}
	
	return(len_used);
}