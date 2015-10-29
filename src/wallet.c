/*

 The MIT License (MIT)

 Copyright (c) 2015 Douglas J. Bakkum

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.

*/

/* Some functions adapted from the Trezor crypto library. */


#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "commander.h"
#include "ripemd160.h"
#include "wallet.h"
#include "random.h"
#include "base58.h"
#include "pbkdf2.h"
#include "utils.h"
#include "flags.h"
#include "sha2.h"
#include "ecc.h"
#include "bip39_english.h"


extern const uint8_t MEM_PAGE_ERASE[MEM_PAGE_LEN];
extern const uint16_t MEM_PAGE_ERASE_2X[MEM_PAGE_LEN];

static char mnemonic[(BIP39_MAX_WORD_LEN + 1) * MAX_SEED_WORDS + 1];
static uint8_t seed[64];
static uint8_t rand_data_32[32];


// Avoid leaving secrets in RAM
static void clear_static_variables(void)
{
    memset(seed, 0, sizeof(seed));
    memset(mnemonic, 0, sizeof(mnemonic));
    memset(rand_data_32, 0, sizeof(rand_data_32));
}


int wallet_split_seed(char **seed_words, const char *message)
{
    int i = 0;
    static char msg[(BIP39_MAX_WORD_LEN + 1) * MAX_SEED_WORDS + 1];

    memset(msg, 0, sizeof(msg));
    snprintf(msg, sizeof(msg), "%s", message);
    seed_words[i] = strtok(msg, " ,");
    for (i = 0; seed_words[i] != NULL; i++) {
        if (i + 1 >= MAX_SEED_WORDS) {
            break;
        }
        seed_words[i + 1] = strtok(NULL, " ,");
    }
    return i;
}


int wallet_seeded(void)
{
    if (!memcmp(memory_master(NULL), MEM_PAGE_ERASE, 32)  ||
            !memcmp(memory_chaincode(NULL), MEM_PAGE_ERASE, 32)) {
        return DBB_ERROR;
    } else {
        return DBB_OK;
    }
}


int wallet_master_from_xpriv(char *src)
{
    if (strlens(src) != 112 - 1) {
        return DBB_ERROR;
    }

    HDNode node;

    clear_static_variables();

    int ret = hdnode_deserialize(src, &node);
    if (ret != DBB_OK) {
        goto exit;
    }

    memory_master(node.private_key);
    memory_chaincode(node.chain_code);

    ret = wallet_seeded();
    if (ret != DBB_OK) {
        ret = DBB_ERROR_MEM;
    }

exit:
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    return ret;
}


int wallet_master_from_mnemonic(char *mnemo, const char *salt)
{
    int ret = DBB_OK;
    HDNode node;

    clear_static_variables();

    if (mnemo == NULL) {
        if (random_bytes(seed, sizeof(seed), 1) == DBB_ERROR) {
            ret = DBB_ERROR_MEM;
            goto exit;
        }
    } else {
        if (strlens(mnemo) > sizeof(mnemonic)) {
            ret = DBB_ERROR;
            goto exit;
        }
        snprintf(mnemonic, sizeof(mnemonic), "%.*s", (int)strlens(mnemo), mnemo);

        if (wallet_mnemonic_check(mnemonic) == DBB_ERROR) {
            ret = DBB_ERROR;
            goto exit;
        }

        if (!strlens(salt)) {
            wallet_mnemonic_to_seed(mnemonic, NULL, seed, 0);
        } else {
            wallet_mnemonic_to_seed(mnemonic, salt, seed, 0);
        }
    }

    if (hdnode_from_seed(seed, sizeof(seed), &node) == DBB_ERROR) {
        ret = DBB_ERROR;
        goto exit;
    }

    memory_master(node.private_key);
    memory_chaincode(node.chain_code);

    ret = wallet_seeded();
    if (ret != DBB_OK) {
        ret = DBB_ERROR_MEM;
    }

exit:
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    return ret;
}


int wallet_generate_key(HDNode *node, const char *keypath, const uint8_t *privkeymaster,
                        const uint8_t *chaincode)
{
    static char delim[] = "/";
    static char prime[] = "phH\'";
    static char digits[] = "0123456789";
    uint64_t idx = 0;
    char *pch, *kp = malloc(strlens(keypath) + 1);

    if (!kp) {
        return DBB_ERROR_MEM;
    }

    if (strlens(keypath) < strlens("m/")) {
        goto err;
    }

    memset(kp, 0, strlens(keypath) + 1);
    memcpy(kp, keypath, strlens(keypath));

    if (kp[0] != 'm' || kp[1] != '/') {
        goto err;
    }

    node->depth = 0;
    node->child_num = 0;
    node->fingerprint = 0;
    memcpy(node->chain_code, chaincode, 32);
    memcpy(node->private_key, privkeymaster, 32);
    hdnode_fill_public_key(node);

    pch = strtok(kp + 2, delim);
    while (pch != NULL) {
        size_t i = 0;
        int prm = 0;
        for ( ; i < strlens(pch); i++) {
            if (strchr(prime, pch[i])) {
                if (i != strlens(pch) - 1) {
                    goto err;
                }
                prm = 1;
            } else if (!strchr(digits, pch[i])) {
                goto err;
            }
        }

        idx = strtoull(pch, NULL, 10);
        if (idx > UINT32_MAX) {
            goto err;
        }

        if (prm) {
            if (hdnode_private_ckd_prime(node, idx) != DBB_OK) {
                goto err;
            }
        } else {
            if (hdnode_private_ckd(node, idx) != DBB_OK) {
                goto err;
            }
        }
        pch = strtok(NULL, delim);
    }
    free(kp);
    return DBB_OK;

err:
    free(kp);
    return DBB_ERROR;
}


void wallet_report_xpriv(const char *keypath, char *xpriv)
{
    HDNode node;
    if (wallet_seeded() == DBB_OK) {
        if (wallet_generate_key(&node, keypath, memory_master(NULL),
                                memory_chaincode(NULL)) == DBB_OK) {
            hdnode_serialize_private(&node, xpriv, 112);
        }
    }
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
}


void wallet_report_xpub(const char *keypath, char *xpub)
{
    HDNode node;
    if (wallet_seeded() == DBB_OK) {
        if (wallet_generate_key(&node, keypath, memory_master(NULL),
                                memory_chaincode(NULL)) == DBB_OK) {
            hdnode_serialize_public(&node, xpub, 112);
        }
    }
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
}


void wallet_report_id(char *id)
{
    uint8_t h[32];
    char xpub[112] = {0};
    wallet_report_xpub("m/", xpub);
    sha256_Raw((uint8_t *)xpub, 112, h);
    memcpy(id, utils_uint8_to_hex(h, 32), 64);
}


int wallet_check_pubkey(const char *address, const char *keypath)
{
    uint8_t pub_key[33];
    char addr[36];
    HDNode node;

    if (strlens(address) != 34) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_checkpub), NULL, DBB_ERR_SIGN_ADDR_LEN);
        goto err;
    }

    if (wallet_seeded() != DBB_OK) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_checkpub), NULL, DBB_ERR_KEY_MASTER);
        goto err;
    }

    if (wallet_generate_key(&node, keypath, memory_master(NULL),
                            memory_chaincode(NULL)) != DBB_OK) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_checkpub), NULL, DBB_ERR_KEY_CHILD);
        goto err;
    }

    ecc_get_public_key33(node.private_key, pub_key);
    wallet_get_address(pub_key, 0, addr, sizeof(addr));

    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    if (strncmp(address, addr, 36)) {
        return DBB_KEY_ABSENT;
    } else {
        return DBB_KEY_PRESENT;
    }

err:
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    return DBB_ERROR;
}


int wallet_sign(const char *message, const char *keypath, int to_hash)
{
    uint8_t data[32];
    uint8_t sig[64];
    uint8_t pub_key[33];
    HDNode node;

    if (!to_hash && strlens(message) != (32 * 2)) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_sign), NULL, DBB_ERR_SIGN_HASH_LEN);
        goto err;
    }

    if (wallet_seeded() != DBB_OK) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_sign), NULL, DBB_ERR_KEY_MASTER);
        goto err;
    }

    if (wallet_generate_key(&node, keypath, memory_master(NULL),
                            memory_chaincode(NULL)) != DBB_OK) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_sign), NULL, DBB_ERR_KEY_CHILD);
        goto err;
    }

    int ret = 0;
    if (to_hash) {
        ret = ecc_sign_double(node.private_key, utils_hex_to_uint8(message), strlens(message) / 2,
                              sig);
    } else {
        memcpy(data, utils_hex_to_uint8(message), 32);
        ret = ecc_sign_digest(node.private_key, data, sig);
    }

    if (ret) {
        commander_clear_report();
        commander_fill_report(cmd_str(CMD_sign), NULL, DBB_ERR_SIGN_ECCLIB);
        goto err;
    }

    ecc_get_public_key33(node.private_key, pub_key);
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    return commander_fill_signature_array(sig, pub_key);

err:
    memset(&node, 0, sizeof(HDNode));
    clear_static_variables();
    return DBB_ERROR;
}


char *wallet_mnemonic_from_data(const uint8_t *data, int len)
{
    if (len % 4 || len < 16 || len > 32) {
        return 0;
    }

    uint8_t bits[32 + 1];

    sha256_Raw(data, len, bits);
    bits[len] = bits[0];
    memcpy(bits, data, len);

    int mlen = len * 3 / 4;
    static char mnemo[24 * 10];

    int i, j;
    char *p = mnemo;
    for (i = 0; i < mlen; i++) {
        int idx = 0;
        for (j = 0; j < 11; j++) {
            idx <<= 1;
            idx += (bits[(i * 11 + j) / 8] & (1 << (7 - ((i * 11 + j) % 8)))) > 0;
        }
        strcpy(p, wordlist[idx]);
        p += strlens(wordlist[idx]);
        *p = (i < mlen - 1) ? ' ' : 0;
        p++;
    }

    return mnemo;
}


int wallet_mnemonic_check(const char *mnemo)
{
    uint32_t i, j, k, ki, bi, n;
    int chksum = 0;
    char *sham[MAX_SEED_WORDS] = {NULL};
    char current_word[10];
    uint8_t bits[32 + 1];

    if (!mnemo) {
        return DBB_ERROR;
    }

    // check number of words
    n = wallet_split_seed(sham, mnemo);
    memset(sham, 0, sizeof(sham));

    if (n != 12 && n != 18 && n != 24) {
        return DBB_ERROR;
    }

    memset(bits, 0, sizeof(bits));
    i = 0;
    bi = 0;
    while (mnemo[i]) {
        j = 0;
        while (mnemo[i] != ' ' && mnemo[i] != 0) {
            if (j >= sizeof(current_word)) {
                return DBB_ERROR;
            }
            current_word[j] = mnemo[i];
            i++;
            j++;
        }
        current_word[j] = 0;
        if (mnemo[i] != 0) {
            i++;
        }
        k = 0;
        for (;;) {
            if (!wordlist[k]) { // word not found
                return DBB_ERROR;
            }
            if (strcmp(current_word, wordlist[k]) == 0) { // word found on index k
                for (ki = 0; ki < 11; ki++) {
                    if (k & (1 << (10 - ki))) {
                        bits[bi / 8] |= 1 << (7 - (bi % 8));
                    }
                    bi++;
                }
                break;
            }
            k++;
        }
    }
    if (bi != n * 11) {
        return DBB_ERROR;
    }
    bits[32] = bits[n * 4 / 3];
    sha256_Raw(bits, n * 4 / 3, bits);
    if (n == 12) {
        chksum = (bits[0] & 0xF0) == (bits[32] & 0xF0); // compare first 4 bits
    } else if (n == 18) {
        chksum = (bits[0] & 0xFC) == (bits[32] & 0xFC); // compare first 6 bits
    } else if (n == 24) {
        chksum = bits[0] == bits[32]; // compare 8 bits
    }

    if (!chksum) {
        return DBB_ERROR;
    }

    return DBB_OK;
}


void wallet_mnemonic_to_seed(const char *mnemo, const char *passphrase,
                             uint8_t s[512 / 8],
                             void (*progress_callback)(uint32_t current, uint32_t total))
{
    static uint8_t salt[8 + SALT_LEN_MAX + 4];
    int saltlen = 0;
    memset(salt, 0, sizeof(salt));
    memcpy(salt, "mnemonic", 8);
    if (passphrase) {
        saltlen = strlens(passphrase);
        snprintf((char *)salt + 8, SALT_LEN_MAX, "%s", passphrase);
    }
    saltlen += 8;
    pbkdf2_hmac_sha512((const uint8_t *)mnemo, strlens(mnemo), salt, saltlen,
                       BIP39_PBKDF2_ROUNDS, s, 512 / 8, progress_callback);
}


int wallet_get_outputs(const char *tx, uint64_t tx_len, char *outputs, int outputs_len)
{
    uint64_t j, in_cnt, out_cnt, n_len, id_start, idx = 0;
    int len;

    idx += 8; // version number

    // Inputs
    if (tx_len < idx + 16) {
        return DBB_ERROR;
    }
    idx += utils_varint_to_uint64(tx + idx, &in_cnt); // inCount
    for (j = 0; j < in_cnt; j++) {
        idx += 64; // prevOutHash
        idx += 8; // preOutIndex
        if (tx_len < idx + 16) {
            return DBB_ERROR;
        }
        idx += utils_varint_to_uint64(tx + idx, &n_len); // scriptSigLen
        idx += n_len * 2; // scriptSig (chars = 2 * bytes)
        idx += 8; // sequence number
    }

    // Outputs
    id_start = idx;
    if (tx_len < idx + 16) {
        return DBB_ERROR;
    }
    idx += utils_varint_to_uint64(tx + idx, &out_cnt); // outCount
    for (j = 0; j < out_cnt; j++) {
        idx += 16; // outValue
        if (tx_len < idx + 16) {
            return DBB_ERROR;
        }
        idx += utils_varint_to_uint64(tx + idx, &n_len); // outScriptLen
        idx += n_len * 2; // outScript (chars = 2 * bytes)
    }
    len = idx - id_start;

    // Return current outputs
    memset(outputs, 0, outputs_len);
    snprintf(outputs, outputs_len, "%.*s", len, tx + id_start);
    return DBB_OK;
}


int wallet_deserialize_output(char *outputs, const char *keypath)
{
    uint64_t j, n_cnt, n_len, idx = 0, outValue;
    char outval[64], outaddr[256], address[36];
    int change_addr_present = 0;
    uint8_t pubkeyhash[20], pub_key33[33];
    HDNode node;

    if (wallet_seeded() != DBB_OK) {
        return DBB_ERROR;
    }

    if (strlens(outputs) < idx + 16) {
        return DBB_ERROR;
    }
    idx += utils_varint_to_uint64(outputs + idx, &n_cnt);
    for (j = 0; j < n_cnt; j++) {
        memset(outval, 0, sizeof(outval));
        strncpy(outval, outputs + idx, 16);
        utils_reverse_hex(outval, 16);
        outValue = strtoull(outval, NULL, 16);
        idx += 16;
        if (strlens(outputs) < idx + 16) {
            return DBB_ERROR;
        }
        idx += utils_varint_to_uint64(outputs + idx, &n_len);

        memset(outval, 0, sizeof(outval));
        memset(outaddr, 0, sizeof(outaddr));
        snprintf(outval, sizeof(outval), "%" PRIu64, outValue);
        snprintf(outaddr, sizeof(outaddr), "%.*s", (int)n_len * 2, outputs + idx);
        idx += n_len * 2; // chars = 2 * bytes

        if (strlens(keypath)) {
            if (wallet_generate_key(&node, keypath, memory_master(NULL),
                                    memory_chaincode(NULL)) != DBB_OK) {
                memset(&node, 0, sizeof(HDNode));
                return DBB_ERROR;
            }
            ecc_get_public_key33(node.private_key, pub_key33);
            wallet_get_pubkeyhash(pub_key33, pubkeyhash);
            wallet_get_address(pub_key33, 0, address, 36);
            if (strstr(outaddr, utils_uint8_to_hex(pubkeyhash, 20))) {
                change_addr_present++;
                continue;
            }
        }
        const char *key[] = {cmd_str(CMD_value), cmd_str(CMD_script), 0};
        const char *value[] = {outval, outaddr, 0};
        int t[] = {DBB_JSON_STRING, DBB_JSON_STRING, DBB_JSON_NONE};
        commander_fill_json_array(key, value, t, CMD_sign);
    }
    memset(&node, 0, sizeof(HDNode));

    if (change_addr_present || n_cnt == 1) {
        return DBB_OK;
    } else {
        // More than 1 output but a change address is not present.
        // Consider this an error in order to prevent MITM attacks.
        return DBB_ERROR;
    }
}


// -- bitcoin formats -- //
// from: github.com/trezor/trezor-crypto

void wallet_get_pubkeyhash(const uint8_t *pub_key, uint8_t *pubkeyhash)
{
    uint8_t h[32];
    if (pub_key[0] == 0x04) {        // uncompressed format
        sha256_Raw(pub_key, 65, h);
    } else if (pub_key[0] == 0x00) { // point at infinity
        sha256_Raw(pub_key, 1, h);
    } else {
        sha256_Raw(pub_key, 33, h);  // expecting compressed format
    }
    ripemd160(h, 32, pubkeyhash);
}

void wallet_get_address_raw(const uint8_t *pub_key, uint8_t version, uint8_t *addr_raw)
{
    addr_raw[0] = version;
    wallet_get_pubkeyhash(pub_key, addr_raw + 1);
}

void wallet_get_address(const uint8_t *pub_key, uint8_t version, char *addr, int addrsize)
{
    uint8_t raw[21];
    wallet_get_address_raw(pub_key, version, raw);
    base58_encode_check(raw, 21, addr, addrsize);
}

void wallet_get_wif(const uint8_t *priv_key, uint8_t version, char *wif, int wifsize)
{
    uint8_t data[34];
    data[0] = version;
    memcpy(data + 1, priv_key, 32);
    data[33] = 0x01;
    base58_encode_check(data, 34, wif, wifsize);
}

