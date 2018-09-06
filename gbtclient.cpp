// (c) 2018 Pttn (https://github.com/Pttn/rieMiner)

#include "global.h"
#include "gbtclient.h"

bool GBTClient::connect(const Arguments& arguments) {
	if (_connected) return false;
	_user = arguments.user();
	_pass = arguments.pass();
	_host = arguments.host();
	_port = arguments.port();
	if (!getWork()) return false;
	_gbtd = GetBlockTemplateData();
	if (!addrToScriptPubKey(arguments.address(), _gbtd.scriptPubKey)) {
		std::cerr << "Invalid payout address! Using donation address instead." << std::endl;
		addrToScriptPubKey("RPttnMeDWkzjqqVp62SdG2ExtCor9w54EB", _gbtd.scriptPubKey);
	}
	_wd = WorkData();
	_pendingSubmissions = std::vector<std::pair<WorkData, uint8_t>>();
	_connected = true;
	return true;
}

void GetBlockTemplateData::coinBaseGen() {
	coinbase = std::vector<uint8_t>();
	uint8_t scriptSig[64], scriptSigLen(26);
	// rieMiner's signature
	hexStrToBin("4d696e65642077697468205074746e2773207269654d696e6572", scriptSig);
	
	// Version [0 -> 3] (01000000)
	coinbase.push_back(1);
	coinbase.push_back(0); coinbase.push_back(0); coinbase.push_back(0);
	// Input Count [4] (01)
	coinbase.push_back(1);
	// 0000000000000000000000000000000000000000000000000000000000000000 (Input TXID [5 -> 36])
	for (uint32_t i(0) ; i < 32 ; i++) coinbase.push_back(0);
	// Input VOUT [37 -> 40] (FFFFFFFF)
	for (uint32_t i(0) ; i < 4 ; i++) coinbase.push_back(0xFF);
	// ScriptSig Size [41]
	coinbase.push_back(4 + scriptSigLen); // Block Height Push (4) + scriptSigLen
	// Block Height Length [42]
	if (height/65536 == 0) {
		if (height/256 == 0) coinbase.push_back(1);
		else coinbase.push_back(2);
	}
	else coinbase.push_back(3);
	// Block Height [43 -> 45]
	coinbase.push_back(height % 256);
	coinbase.push_back((height/256) % 256);
	coinbase.push_back((height/65536) % 256);
	// ScriptSig [46 -> 46 + scriptSigLen = s] s = 72
	for (uint32_t i(0) ; i < scriptSigLen ; i++) coinbase.push_back(scriptSig[i]);
	// Input Sequence [s -> s + 3] (FFFFFFFF)
	for (uint32_t i(0) ; i < 4 ; i++) coinbase.push_back(0xFF);
	
	// Output Count [s + 4]
	coinbase.push_back(1);
	// Output Value [s + 5 -> s + 12]
	uint64_t coinbasevalue2(coinbasevalue);
	for (uint32_t i(0) ; i < 8 ; i++) {
		coinbase.push_back(coinbasevalue2 % 256);
		coinbasevalue2 /= 256;
	}
	coinbase.push_back(25); // Output Length [s + 13]
	coinbase.push_back(0x76); // OP_DUP [s + 14]
	coinbase.push_back(0xA9); // OP_HASH160 [s + 15]
	coinbase.push_back(0x14); // Bytes Pushed on Stack [s + 16]
	// ScriptPubKey (for payout address) [s + 17 -> s + 36]
	for (uint32_t i(0) ; i < 20 ; i++) coinbase.push_back(scriptPubKey[i]);
	coinbase.push_back(0x88); // OP_EQUALVERIFY [s + 37]
	coinbase.push_back(0xAC); // OP_CHECKSIG [s + 38]
	// Lock Time  [s + 39 -> s + 42] (00000000)
	for (uint32_t i(0) ; i < 4 ; i++) coinbase.push_back(0);
}

bool GBTClient::getWork() {
	json_t *jsonGbt(NULL), *jsonRes(NULL), *jsonTxs(NULL);
	jsonGbt = sendRPCCall(_curl, "{\"method\": \"getblocktemplate\", \"params\": [], \"id\": 0}\n");
	jsonRes = json_object_get(jsonGbt, "result");
	jsonTxs = json_object_get(jsonRes, "transactions");
	
	// Failure to GetBlockTemplate
	if (jsonGbt == NULL || jsonRes == NULL || jsonTxs == NULL ) return false;
	
	uint32_t oldHeight(_wd.height);
	uint8_t bitsTmp[4];
	hexStrToBin(json_string_value(json_object_get(jsonRes, "bits")), bitsTmp);
	_gbtd.bh = BlockHeader();
	_gbtd.transactions = std::string();
	_gbtd.txHashes = std::vector<std::array<uint32_t, 8>>();
	
	// Extract and build GetBlockTemplate data
	_gbtd.bh.version = json_integer_value(json_object_get(jsonRes, "version"));
	hexStrToBin(json_string_value(json_object_get(jsonRes, "previousblockhash")), (uint8_t*) &_gbtd.bh.previousblockhash);
	_gbtd.height = json_integer_value(json_object_get(jsonRes, "height"));
	_gbtd.coinbasevalue = json_integer_value(json_object_get(jsonRes, "coinbasevalue"));
	_gbtd.bh.bits = ((uint32_t*) bitsTmp)[0];
	_gbtd.bh.curtime = json_integer_value(json_object_get(jsonRes, "curtime"));
	_gbtd.coinBaseGen();
	_gbtd.transactions += binToHexStr(_gbtd.coinbase.data(), _gbtd.coinbase.size());
	_gbtd.txHashes.push_back(_gbtd.coinBaseHash());
	for (uint32_t i(0) ; i < json_array_size(jsonTxs) ; i++) {
		std::array<uint32_t, 8> txHash;
		uint8_t txHashTmp[32], txHashInvTmp[32];
		hexStrToBin(json_string_value(json_object_get(json_array_get(jsonTxs, i), "hash")), txHashInvTmp);
		for (uint16_t j(0) ; j < 32 ; j++) txHashTmp[j] = txHashInvTmp[31 - j];
		for (uint32_t j(0) ; j < 8 ; j++) txHash[j] = ((uint32_t*) txHashTmp)[j];
		_gbtd.transactions += json_string_value(json_object_get(json_array_get(jsonTxs, i), "data"));
		_gbtd.txHashes.push_back(txHash);
	}
	_gbtd.merkleRootGen();
	
	memcpy(&_wd.bh, &_gbtd.bh, 128);
	
	// Notify when the network found a block
	if (oldHeight != _gbtd.height) {
		if (oldHeight != 0) {
			stats.printTime();
			if (_wd.height - stats.blockHeightAtDifficultyChange != 0) {
				std::cout << " Blockheight = " << _gbtd.height - 1 << ", average "
				          << FIXED(1) << timeSince(stats.lastDifficultyChange)/(_wd.height - stats.blockHeightAtDifficultyChange)
				          << " s, difficulty = " << stats.difficulty << std::endl;
			}
			else
				std::cout << " Blockheight = " << _gbtd.height - 1 << ", new difficulty = " << stats.difficulty << std::endl;
		}
		else stats.blockHeightAtDifficultyChange = _gbtd.height;
	}
	
	_wd.height = _gbtd.height;
	_wd.bh.bits = swab32(_wd.bh.bits);
	_wd.targetCompact = getCompact(_wd.bh.bits);
	_wd.transactions  = _gbtd.transactions;
	_wd.txCount       = _gbtd.txHashes.size();
	// Change endianness for mining (will revert back when submit share)
	for (uint8_t i(0) ; i < 32; i++) ((uint8_t*) _wd.bh.previousblockhash)[i] = ((uint8_t*) _gbtd.bh.previousblockhash)[31 - i];
	
	json_decref(jsonGbt);
	json_decref(jsonRes);
	json_decref(jsonTxs);
	
	return true;
}

void GBTClient::sendWork(const std::pair<WorkData, uint8_t>& share) const {
	WorkData wdToSend(share.first);
	
	json_t *jsonObj = NULL, *res = NULL, *reason = NULL;
	
	std::ostringstream oss;
	std::string req;// GWDSIZE
	oss << "{\"method\": \"submitblock\", \"params\": [\"" << binToHexStr(&wdToSend.bh, (32 + 256 + 256 + 32 + 64 + 256)/8);
	// Using the Variable Length Integer format
	if (wdToSend.txCount < 0xFD)
		oss << binToHexStr((uint8_t*) &wdToSend.txCount, 1);
	else // Having more than 65535 transactions is currently impossible
		oss << "fd" << binToHexStr((uint8_t*) &wdToSend.txCount, 2);
	oss << wdToSend.transactions << "\"], \"id\": 0}\n";
	req = oss.str();
	jsonObj = sendRPCCall(_curl, req);
	
	uint8_t k(share.second);
	if (k >= 4) {
		stats.printTime();
		std::cout << " 4-tuple found";
		if (k == 4) std::cout << std::endl;
	}
	if (k >= 5) {
		std::cout << "... Actually it was a 5-tuple";
		if (k == 5) std::cout << std::endl;
	}
	if (k >= 6) {
		std::cout << "... No, no... A 6-tuple = BLOCK!!!!!!" << std::endl;
		std::cout << "Sent: " << req;
		if (jsonObj == NULL)
			std::cerr << "Failure submiting block :|" << std::endl;
		else {
			res = json_object_get(jsonObj, "result");
			if (json_is_null(res)) std::cout << "Submission accepted :D !" << std::endl;
			else {
				std::cout << "Submission rejected :| ! ";
				if (reason == NULL) std::cout << "No reason given" << std::endl;
				else std::cout << "Reason: " << reason << std::endl;
			}
		}
	}
	
	if (jsonObj != NULL) json_decref(jsonObj);
}
