/*
 * This file is part of the TREZOR project.
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

/* === Includes ============================================================ */

#include <msg_dispatch.h>
#include <ecdsa.h>
#include <secp256k1.h>
#include <crypto.h>
#include <layout.h>
#include <confirm_sm.h>

#include "signing.h"
#include "fsm.h"
#include "transaction.h"
#include "coins.h"
#include "home_sm.h"
#include "app_confirm.h"

/* === Private Variables =================================================== */

static uint32_t inputs_count;
static uint32_t outputs_count;
static const CoinType *coin;
static const HDNode *root;
static HDNode node;
static bool signing = false;
static uint32_t idx1, idx2;
static TxRequest resp;
static TxInputType input;
static TxOutputBinType bin_output;
static TxStruct to, tp, ti;
static SHA256_CTX tc;
static uint8_t hash[32], hash_check[32], privkey[32], pubkey[33], sig[64];
static uint64_t to_spend, spending, change_spend;
static bool multisig_fp_set, multisig_fp_mismatch;
static uint8_t multisig_fp[32];

/* === Variables =========================================================== */

enum {
	STAGE_REQUEST_1_INPUT,
	STAGE_REQUEST_2_PREV_META,
	STAGE_REQUEST_2_PREV_INPUT,
	STAGE_REQUEST_2_PREV_OUTPUT,
	STAGE_REQUEST_3_OUTPUT,
	STAGE_REQUEST_4_INPUT,
	STAGE_REQUEST_4_OUTPUT,
	STAGE_REQUEST_5_OUTPUT
} signing_stage;
const uint32_t version = 1;
const uint32_t lock_time = 0;

/* === Private Functions =================================================== */

/*
 * check_valid_output_address() - Checks the sanity of an output
 *
 * INPUT
 *     - stor_config: storage config
 * OUTPUT
 *     true/false status
 *
 */
static bool check_valid_output_address(TxOutputType *tx_out)
{
    bool ret_val = false;

    switch(tx_out->address_type)
    {
        case OutputAddressType_SPEND:
            if(tx_out->has_address)
            {
                /* valid address type */
                ret_val = true;
            }

            break;

        case OutputAddressType_TRANSFER:
        case OutputAddressType_CHANGE:
            if(tx_out->address_n_count > 0)
            {
                /* valid address type */
                ret_val = true;
            }

            break;
    }

    return(ret_val);
}

/* === Functions =========================================================== */

/*
Workflow of streamed signing
The STAGE_ constants describe the signing_stage when request is sent.
I - input
O - output
Phase1 - check inputs, previous transactions, and outputs
       - ask for confirmations
       - check fee
=========================================================
foreach I (idx1):
    Request I                                                         STAGE_REQUEST_1_INPUT
    Add I to TransactionChecksum
    Calculate amount of I:
        Request prevhash I, META                                      STAGE_REQUEST_2_PREV_META
        foreach prevhash I (idx2):
            Request prevhash I                                        STAGE_REQUEST_2_PREV_INPUT
        foreach prevhash O (idx2):
            Request prevhash O                                        STAGE_REQUEST_2_PREV_OUTPUT
            Add amount of prevhash O (which is amount of I)
        Calculate hash of streamed tx, compare to prevhash I
foreach O (idx1):
    Request O                                                         STAGE_REQUEST_3_OUTPUT
    Add O to TransactionChecksum
    Display output
    Ask for confirmation
Check tx fee
Ask for confirmation
Phase2: sign inputs, check that nothing changed
===============================================
foreach I (idx1):  // input to sign
    foreach I (idx2):
        Request I                                                     STAGE_REQUEST_4_INPUT
        If idx1 == idx2
        Remember key for signing
            Fill scriptsig
        Add I to StreamTransactionSign
        Add I to TransactionChecksum
    foreach O (idx2):
        Request O                                                     STAGE_REQUEST_4_OUTPUT
        Add O to StreamTransactionSign
        Add O to TransactionChecksum
    Compare TransactionChecksum with checksum computed in Phase 1
    If different:
        Failure
    Sign StreamTransactionSign
    Return signed chunk
foreach O (idx1):
    Request O                                                         STAGE_REQUEST_5_OUTPUT
    Rewrite change address
    Return O
*/

void send_req_1_input(void)
{
	signing_stage = STAGE_REQUEST_1_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_meta(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_META;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXMETA;
	resp.has_details = true;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, input.prev_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_input(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, resp.details.tx_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_2_prev_output(void)
{
	signing_stage = STAGE_REQUEST_2_PREV_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	resp.details.has_tx_hash = true;
	resp.details.tx_hash.size = input.prev_hash.size;
	memcpy(resp.details.tx_hash.bytes, input.prev_hash.bytes, resp.details.tx_hash.size);
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_3_output(void)
{
	signing_stage = STAGE_REQUEST_3_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_4_input(void)
{
	signing_stage = STAGE_REQUEST_4_INPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXINPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_4_output(void)
{
	signing_stage = STAGE_REQUEST_4_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx2;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_5_output(void)
{
	signing_stage = STAGE_REQUEST_5_OUTPUT;
	resp.has_request_type = true;
	resp.request_type = RequestType_TXOUTPUT;
	resp.has_details = true;
	resp.details.has_request_index = true;
	resp.details.request_index = idx1;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void send_req_finished(void)
{
	resp.has_request_type = true;
	resp.request_type = RequestType_TXFINISHED;
	msg_write(MessageType_MessageType_TxRequest, &resp);
}

void signing_init(uint32_t _inputs_count, uint32_t _outputs_count, const CoinType *_coin, const HDNode *_root)
{
	inputs_count = _inputs_count;
	outputs_count = _outputs_count;
	coin = _coin;
	root = _root;

	idx1 = 0;
	to_spend = 0;
	spending = 0;
	change_spend = 0;
	memset(&input, 0, sizeof(TxInputType));
	memset(&resp, 0, sizeof(TxRequest));

	signing = true;

	multisig_fp_set = false;
	multisig_fp_mismatch = false;

	tx_init(&to, inputs_count, outputs_count, version, lock_time, false);
	sha256_Init(&tc);
	sha256_Update(&tc, (const uint8_t *)&inputs_count, sizeof(inputs_count));
	sha256_Update(&tc, (const uint8_t *)&outputs_count, sizeof(outputs_count));
	sha256_Update(&tc, (const uint8_t *)&version, sizeof(version));
	sha256_Update(&tc, (const uint8_t *)&lock_time, sizeof(lock_time));

	animating_progress_handler();

	send_req_1_input();
}

void signing_txack(TransactionType *tx)
{
	if (!signing) {
		fsm_sendFailure(FailureType_Failure_UnexpectedMessage, "Not in Signing mode");
		go_home();
		return;
	}

	static int update_ctr = 0;
	if (update_ctr++ == 20) {
		animating_progress_handler();
		update_ctr = 0;
	}

	int co;
	memset(&resp, 0, sizeof(TxRequest));

	switch (signing_stage) {
		case STAGE_REQUEST_1_INPUT:
			/* compute multisig fingerprint */
			/* (if all input share the same fingerprint, outputs having the same fingerprint will be considered as change outputs) */
			if (tx->inputs[0].script_type == InputScriptType_SPENDMULTISIG) {
				if (tx->inputs[0].has_multisig && !multisig_fp_mismatch) {
					if (multisig_fp_set) {
						uint8_t h[32];
						if (cryptoMultisigFingerprint(&(tx->inputs[0].multisig), h) == 0) {
							fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
							signing_abort();
							return;
						}
						if (memcmp(multisig_fp, h, 32) != 0) {
							multisig_fp_mismatch = true;
						}
					} else {
						if (cryptoMultisigFingerprint(&(tx->inputs[0].multisig), multisig_fp) == 0) {
							fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
							signing_abort();
							return;
						}
						multisig_fp_set = true;
					}
				}
			} else { // InputScriptType_SPENDADDRESS
				multisig_fp_mismatch = true;
			}
			sha256_Update(&tc, (const uint8_t *)tx->inputs, sizeof(TxInputType));
			memcpy(&input, tx->inputs, sizeof(TxInputType));
			send_req_2_prev_meta();
			return;
		case STAGE_REQUEST_2_PREV_META:
			tx_init(&tp, tx->inputs_cnt, tx->outputs_cnt, tx->version, tx->lock_time, false);
			idx2 = 0;
			send_req_2_prev_input();
			return;
		case STAGE_REQUEST_2_PREV_INPUT:
			if (!tx_serialize_input_hash(&tp, tx->inputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize input");
				signing_abort();
				return;
			}
			if (idx2 < tp.inputs_len - 1) {
				idx2++;
				send_req_2_prev_input();
			} else {
				idx2 = 0;
				send_req_2_prev_output();
			}
			return;
		case STAGE_REQUEST_2_PREV_OUTPUT:
			if (!tx_serialize_output_hash(&tp, tx->bin_outputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize output");
				signing_abort();
				return;
			}
			if (idx2 == input.prev_index) {
				to_spend += tx->bin_outputs[0].amount;
			}
			if (idx2 < tp.outputs_len - 1) {
				/* Check prevtx of next input */
				idx2++;
				send_req_2_prev_output();
			} else {
				/* Check next output */
				tx_hash_final(&tp, hash, true);
				if (memcmp(hash, input.prev_hash.bytes, 32) != 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Encountered invalid prevhash");
					signing_abort();
					return;
				}
				if (idx1 < inputs_count - 1) {
					idx1++;
					send_req_1_input();
				} else {
					idx1 = 0;
					send_req_3_output();
				}
			}
			return;
		case STAGE_REQUEST_3_OUTPUT:
		{
			/* Downloaded output idx1 the first time.
			 *  Add it to transaction check
			 *  Ask for permission.
			 */
			bool is_change = false;

			if (tx->outputs[0].script_type == OutputScriptType_PAYTOMULTISIG &&
			    tx->outputs[0].has_multisig &&
			    multisig_fp_set && !multisig_fp_mismatch) {
				uint8_t h[32];
				if (cryptoMultisigFingerprint(&(tx->outputs[0].multisig), h) == 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Error computing multisig fingeprint");
					signing_abort();
					return;
				}
				if (memcmp(multisig_fp, h, 32) == 0) {
					is_change = true;
				}
                        } else {
                            if(tx->outputs[0].has_address_type) {
                                if(check_valid_output_address(tx->outputs) == false) {
                                    fsm_sendFailure(FailureType_Failure_Other, "Invalid output address type");
                                    signing_abort();
                                    return;
                                }

                                if(tx->outputs[0].script_type == OutputScriptType_PAYTOADDRESS &&
                                        tx->outputs[0].address_n_count > 0 &&
                                        tx->outputs[0].address_type == OutputAddressType_CHANGE) {
                                    is_change = true;
                                }
                            }
                            else if(tx->outputs[0].script_type == OutputScriptType_PAYTOADDRESS &&
                                    tx->outputs[0].address_n_count > 0) {
                                is_change = true;
                            }
                        }

			if (is_change) {
				if (change_spend == 0) { // not set
					change_spend = tx->outputs[0].amount;
				} else {
					fsm_sendFailure(FailureType_Failure_Other, "Only one change output allowed");
					signing_abort();
					return;
			    }
			}

			spending += tx->outputs[0].amount;
			co = compile_output(coin, root, tx->outputs, &bin_output, !is_change);
			if (!is_change) {
				animating_progress_handler();
			}
			if (co < 0) {
				fsm_sendFailure(FailureType_Failure_Other, "Signing cancelled by user");
				signing_abort();
				return;
			} else if (co == 0) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to compile output");
				signing_abort();
				return;
			}
			sha256_Update(&tc, (const uint8_t *)&bin_output, sizeof(TxOutputBinType));
			if (idx1 < outputs_count - 1) {
				idx1++;
				send_req_3_output();
			} else {
                            sha256_Final(hash_check, &tc);
                            // check fees
                            if (spending > to_spend) {
			        fsm_sendFailure(FailureType_Failure_NotEnoughFunds, "Not enough funds");
                                go_home();
				return;
                            }
                            uint64_t fee = to_spend - spending;
                            uint32_t tx_est_size = transactionEstimateSizeKb(inputs_count, outputs_count);
                            char total_amount_str[32];
		            char fee_str[32];

		            coin_amnt_to_str(coin, fee, fee_str, sizeof(fee_str));

                            if(fee > (uint64_t)tx_est_size * coin->maxfee_kb) {
			        if (!confirm(ButtonRequestType_ButtonRequest_FeeOverThreshold,
		                        "Confirm Fee", "%s", fee_str)) {
		                    fsm_sendFailure(FailureType_Failure_ActionCancelled, "Fee over threshold. Signing cancelled.");
		                    go_home();
		                    return;
		                }
                                animating_progress_handler();
                            }
                            // last confirmation
                            coin_amnt_to_str(coin, to_spend - change_spend, total_amount_str, sizeof(total_amount_str));

		            if(!confirm_transaction(total_amount_str, fee_str))
		            {
		                fsm_sendFailure(FailureType_Failure_ActionCancelled, "Signing cancelled by user");
		                signing_abort();
		                return;
		            }
		            // Everything was checked, now phase 2 begins and the transaction is signed.
			    animating_progress_handler();
			    idx1 = 0;
			    idx2 = 0;
			    send_req_4_input();
			}
			return;
		}
		case STAGE_REQUEST_4_INPUT:
			if (idx2 == 0) {
				tx_init(&ti, inputs_count, outputs_count, version, lock_time, true);
				sha256_Init(&tc);
				sha256_Update(&tc, (const uint8_t *)&inputs_count, sizeof(inputs_count));
				sha256_Update(&tc, (const uint8_t *)&outputs_count, sizeof(outputs_count));
				sha256_Update(&tc, (const uint8_t *)&version, sizeof(version));
				sha256_Update(&tc, (const uint8_t *)&lock_time, sizeof(lock_time));
				memset(privkey, 0, 32);
				memset(pubkey, 0, 33);
			}
			sha256_Update(&tc, (const uint8_t *)tx->inputs, sizeof(TxInputType));
			if (idx2 == idx1) {
				memcpy(&input, tx->inputs, sizeof(TxInputType));
				memcpy(&node, root, sizeof(HDNode));
				if (hdnode_private_ckd_cached(&node, tx->inputs[0].address_n, tx->inputs[0].address_n_count) == 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Failed to derive private key");
					signing_abort();
					return;
				}
				if (tx->inputs[0].script_type == InputScriptType_SPENDMULTISIG) {
					if (!tx->inputs[0].has_multisig) {
						fsm_sendFailure(FailureType_Failure_Other, "Multisig info not provided");
						signing_abort();
						return;
					}
					tx->inputs[0].script_sig.size = compile_script_multisig(&(tx->inputs[0].multisig), tx->inputs[0].script_sig.bytes);
				} else { // SPENDADDRESS
					ecdsa_get_pubkeyhash(node.public_key, hash);
					tx->inputs[0].script_sig.size = compile_script_sig(coin->address_type, hash, tx->inputs[0].script_sig.bytes);
				}
				if (tx->inputs[0].script_sig.size == 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Failed to compile input");
					signing_abort();
					return;
				}
				memcpy(privkey, node.private_key, 32);
				memcpy(pubkey, node.public_key, 33);
			} else {
				tx->inputs[0].script_sig.size = 0;
			}
			if (!tx_serialize_input_hash(&ti, tx->inputs)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize input");
				signing_abort();
				return;
			}
			if (idx2 < inputs_count - 1) {
				idx2++;
				send_req_4_input();
			} else {
				idx2 = 0;
				send_req_4_output();
			}
			return;
		case STAGE_REQUEST_4_OUTPUT:
			co = compile_output(coin, root, tx->outputs, &bin_output, false);
			if (co < 0) {
				fsm_sendFailure(FailureType_Failure_Other, "Signing cancelled by user");
				signing_abort();
				return;
			} else if (co == 0) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to compile output");
				signing_abort();
				return;
			}
			sha256_Update(&tc, (const uint8_t *)&bin_output, sizeof(TxOutputBinType));
			if (!tx_serialize_output_hash(&ti, &bin_output)) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize output");
				signing_abort();
				return;
			}
			if (idx2 < outputs_count - 1) {
				idx2++;
				send_req_4_output();
			} else {
				sha256_Final(hash, &tc);
				if (memcmp(hash, hash_check, 32) != 0) {
					fsm_sendFailure(FailureType_Failure_Other, "Transaction has changed during signing");
					signing_abort();
					return;
				}
				tx_hash_final(&ti, hash, false);
				resp.has_serialized = true;
				resp.serialized.has_signature_index = true;
				resp.serialized.signature_index = idx1;
				resp.serialized.has_signature = true;
				resp.serialized.has_serialized_tx = true;
				ecdsa_sign_digest(&secp256k1, privkey, hash, sig, 0);
				resp.serialized.signature.size = ecdsa_sig_to_der(sig, resp.serialized.signature.bytes);
				if (input.script_type == InputScriptType_SPENDMULTISIG) {
					if (!input.has_multisig) {
						fsm_sendFailure(FailureType_Failure_Other, "Multisig info not provided");
						signing_abort();
						return;
					}
					// fill in the signature
					int pubkey_idx = cryptoMultisigPubkeyIndex(&(input.multisig), pubkey);
					if (pubkey_idx < 0) {
						fsm_sendFailure(FailureType_Failure_Other, "Pubkey not found in multisig script");
						signing_abort();
						return;
					}
					memcpy(input.multisig.signatures[pubkey_idx].bytes, resp.serialized.signature.bytes, resp.serialized.signature.size);
					input.multisig.signatures[pubkey_idx].size = resp.serialized.signature.size;
					input.script_sig.size = serialize_script_multisig(&(input.multisig), input.script_sig.bytes);
					if (input.script_sig.size == 0) {
						fsm_sendFailure(FailureType_Failure_Other, "Failed to serialize multisig script");
						signing_abort();
						return;
					}
				} else { // SPENDADDRESS
					input.script_sig.size = serialize_script_sig(resp.serialized.signature.bytes, resp.serialized.signature.size, pubkey, 33, input.script_sig.bytes);
				}
				resp.serialized.serialized_tx.size = tx_serialize_input(&to, &input, resp.serialized.serialized_tx.bytes);
				// since this took a longer time, update progress
				animating_progress_handler();
				update_ctr = 0;
				if (idx1 < inputs_count - 1) {
					idx1++;
					idx2 = 0;
					send_req_4_input();
				} else {
					idx1 = 0;
					send_req_5_output();
				}
			}
			return;
		case STAGE_REQUEST_5_OUTPUT:
			if (compile_output(coin, root, tx->outputs, &bin_output,false) <= 0) {
				fsm_sendFailure(FailureType_Failure_Other, "Failed to compile output");
				signing_abort();
				return;
			}
			resp.has_serialized = true;
			resp.serialized.has_serialized_tx = true;
			resp.serialized.serialized_tx.size = tx_serialize_output(&to, &bin_output, resp.serialized.serialized_tx.bytes);
			if (idx1 < outputs_count - 1) {
				idx1++;
				send_req_5_output();
			} else {
				send_req_finished();
				signing_abort();
			}
			return;
	}

	fsm_sendFailure(FailureType_Failure_Other, "Signing error");
	signing_abort();
}

void signing_abort(void)
{
	if (signing) {
		go_home();
		signing = false;
	}
}
