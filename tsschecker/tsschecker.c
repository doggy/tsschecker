//
//  ipswme.c
//  tsschecker
//
//  Created by tihmstar on 07.01.16.
//  Copyright © 2016 tihmstar. All rights reserved.
//

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include "tsschecker.h"
#include "jsmn.h"
#include "download.h"
#include <libfragmentzip/libfragmentzip.h>
#include <libirecovery.h>
#include "tss.h"

#ifdef __APPLE__
#   include <CommonCrypto/CommonDigest.h>
#   define SHA1(d, n, md) CC_SHA1(d, n, md)
#else
#   include <openssl/sha.h>
#endif // __APPLE__

#define FIRMWARE_JSON_URL "https://api.ipsw.me/v2.1/firmwares.json/condensed"
#define FIRMWARE_OTA_JSON_URL "https://api.ipsw.me/v2.1/ota.json/condensed"
#define BBGCID_JSON_URL "http://api.tihmstar.net/bbgcid?condensed=1"

#warning TODO verify these values are actually correct for all devices (iPhone7)
#define NONCELEN_BASEBAND 20 
#define NONCELEN_SEP      20



#ifdef WIN32
#include <windows.h>
#define __mkdir(path, mode) mkdir(path)
static int win_path_didinit = 0;
static const char *win_paths[4];

enum paths{
    kWINPathTSSCHECKER,
    kWINPathBBGCID,
    kWINPathOTA,
    kWINPathFIRMWARE
};

static const char *win_pathvars[]={
    "\\tsschecker",
    "\\bbgcid.json",
    "\\ota.json",
    "\\firmware.json"
};

static const char *win_path_get(enum paths path){
    if (!win_path_didinit) memset(win_paths, 0, sizeof(win_paths));
    if (win_paths[path]) return win_paths[path];
    
    const char *tmp = getenv("TMP");
    if (tmp && *tmp){
        size_t len = strlen(tmp) + strlen(win_pathvars[path]) + 1;
        win_paths[path] = (char *)malloc(len);
        memset((char*)win_paths[path], '\0', len);
        strncat((char*)win_paths[path], tmp, strlen(tmp));
        strncat((char*)win_paths[path], win_pathvars[path], strlen(win_pathvars[path]));
        return win_paths[path];
    }
    
    error("DEBUG: tmp=%s win_paths[path]=%s\n",tmp,win_paths[path]);
    error("FATAL could not get TMP path. exiting!\n");
    exit(123);
    return NULL;
}

#define MANIFEST_SAVE_PATH win_path_get(kWINPathTSSCHECKER)
#define BBGCID_JSON_PATH win_path_get(kWINPathBBGCID)
#define FIRMWARE_OTA_JSON_PATH win_path_get(kWINPathOTA)
#define FIRMWARE_JSON_PATH win_path_get(kWINPathFIRMWARE)
#define DIRECTORY_DELIMITER_STR "\\"
#define DIRECTORY_DELIMITER_CHR '\\'

#else

#define MANIFEST_SAVE_PATH "/tmp/tsschecker"
#define BBGCID_JSON_PATH "/tmp/bbgcid.json"
#define FIRMWARE_OTA_JSON_PATH "/tmp/ota.json"
#define FIRMWARE_JSON_PATH "/tmp/firmware.json"
#define DIRECTORY_DELIMITER_STR "/"
#define DIRECTORY_DELIMITER_CHR '/'


#include <sys/stat.h>
#define __mkdir(path, mode) mkdir(path, mode)

#endif

#pragma mark getJson functions

int dbglog = 0;
int print_tss_request = 0;
int print_tss_response = 0;
int nocache = 0;
int save_shshblobs = 0;
const char *shshSavePath = "."DIRECTORY_DELIMITER_STR;

char *getBBCIDJson(){
    info("[TSSC] opening bbgcid.json\n");
    FILE *f = fopen(BBGCID_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(BBGCID_JSON_URL, BBGCID_JSON_PATH);
        f = fopen(BBGCID_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fJson[fsize] = '\0';
    fclose(f);
    return fJson;
}

char *getFirmwareJson(){
    info("[TSSC] opening firmware.json\n");
    FILE *f = fopen(FIRMWARE_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(FIRMWARE_JSON_URL, FIRMWARE_JSON_PATH);
        f = fopen(FIRMWARE_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fclose(f);
    return fJson;
}

char *getOtaJson(){
    info("[TSSC] opening ota.json\n");
    FILE *f = fopen(FIRMWARE_OTA_JSON_PATH, "rb");
    if (!f || nocache){
        downloadFile(FIRMWARE_OTA_JSON_URL, FIRMWARE_OTA_JSON_PATH);
        f = fopen(FIRMWARE_OTA_JSON_PATH, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *fJson = malloc(fsize + 1);
    fread(fJson, fsize, 1, f);
    fclose(f);
    return fJson;
}

#pragma mark more get functions

const char *getBoardconfigFromModel(const char *model){
    const char *rt = NULL;
    irecv_device_t table = irecv_devices_get_all();
    //iterate through table until find correct entry
    //table is terminated with {NULL, NULL, -1, -1} entry, return that if device not found
    while (table->product_type){
        if (strncmp(model, table->product_type, strlen(model)) == 0){
            if (rt){
                warning("can't unambiguously map model to boardconfig for device %s\n",model);
                return NULL;
            }else
                rt = table->hardware_model;
        }
        
        table++;
    }
    
    return rt;
}

const char *getModelFromBoardconfig(const char *boardconfig){
    const char *rt = NULL;
    irecv_device_t table = irecv_devices_get_all();
    //iterate through table until find correct entry
    //table is terminated with {NULL, NULL, -1, -1} entry, return that if device not found
    while (table->product_type){
        if (strncmp(boardconfig, table->hardware_model, strlen(boardconfig)) == 0){
            if (rt){
                warning("can't unambiguously map boardconfig to model for device %s\n",boardconfig);
                return NULL;
            }else
                rt = table->product_type;
        }
        
        table++;
    }
    
    return rt;
}

plist_t getBuildidentityWithBoardconfig(plist_t buildManifest, const char *boardconfig, int isUpdateInstall){
    plist_t rt = NULL;
#define reterror(a ... ) {error(a); rt = NULL; goto error;}
    
    plist_t buildidentities = plist_dict_get_item(buildManifest, "BuildIdentities");
    if (!buildidentities || plist_get_node_type(buildidentities) != PLIST_ARRAY){
        reterror("[TSSR] Error: could not get BuildIdentities\n");
    }
    for (int i=0; i<plist_array_get_size(buildidentities); i++) {
        rt = plist_array_get_item(buildidentities, i);
        if (!rt || plist_get_node_type(rt) != PLIST_DICT){
            reterror("[TSSR] Error: could not get id%d\n",i);
        }
        plist_t infodict = plist_dict_get_item(rt, "Info");
        if (!infodict || plist_get_node_type(infodict) != PLIST_DICT){
            reterror("[TSSR] Error: could not get infodict\n");
        }
        plist_t RestoreBehavior = plist_dict_get_item(infodict, "RestoreBehavior");
        if (!RestoreBehavior || plist_get_node_type(RestoreBehavior) != PLIST_STRING){
            reterror("[TSSR] Error: could not get RestoreBehavior\n");
        }
        char *string = NULL;
        plist_get_string_val(RestoreBehavior, &string);
        if (strncmp(string, (isUpdateInstall ? "Update" : "Erase"), strlen(string)) != 0)
            rt = NULL;
        
        plist_t DeviceClass = plist_dict_get_item(infodict, "DeviceClass");
        if (!DeviceClass || plist_get_node_type(DeviceClass) != PLIST_STRING){
            reterror("[TSSR] Error: could not get DeviceClass\n");
        }
        plist_get_string_val(DeviceClass, &string);
        if (strncmp(string, boardconfig, strlen(boardconfig)) != 0)
            rt = NULL;
        else
            break;
        
    }
    
error:
    return rt;
#undef reterror
}

plist_t getBuildidentity(plist_t buildManifest, const char *model, int isUpdateInstall){
    plist_t rt = NULL;
#define reterror(a ... ) {error(a); rt = NULL; goto error;}
    
    const char *boardconfig = getBoardconfigFromModel(model);
    if (!boardconfig)
        reterror("[TSSR] cant find boardconfig for device=%s please manuall use --boardconfig\n",model);
    
    rt = getBuildidentityWithBoardconfig(buildManifest, boardconfig, isUpdateInstall);
    
error:
    return rt;
#undef reterror
}


#pragma mark json functions

void printJString(jsmntok_t *str, char * firmwarejson){
    for (int j=0; j<str->end-str->start; j++) {
        putchar(*(firmwarejson+str->start + j));
    }
    putchar('\n');
}

jsmntok_t *objectForKey(jsmntok_t *tokens, const char *firmwareJson, const char*key){
    
    jsmntok_t *dictElements = tokens->value;
    for (jsmntok_t *tmp = dictElements; ; tmp = tmp->next) {
        
        if (strncmp(key, firmwareJson + tmp->start, strlen(key)) == 0) return tmp;
        
        if (tmp->next == dictElements) break;
    }
    
    return NULL;
}

int parseTokens(const char *json, jsmntok_t **tokens){
    jsmn_parser parser;
    jsmn_init(&parser);
    
    log("[JSON] counting elements\n");
    unsigned int tokensCnt = jsmn_parse(&parser, json, strlen(json), NULL, 0);
    
    *tokens = (jsmntok_t*)malloc(sizeof(jsmntok_t) * tokensCnt);
    jsmn_init(&parser);
    log("[JSON] parsing elements\n");
    return jsmn_parse(&parser, json, strlen(json), *tokens, tokensCnt);
}

#pragma mark get functions

char *getFirmwareUrl(t_devicevals *devVals, t_iosVersion *versVals, const char *firmwarejson, jsmntok_t *tokens){
    
    jsmntok_t *firmwares = getFirmwaresForDevice(devVals->deviceModel, firmwarejson, tokens, versVals->isOta);
    
    for (jsmntok_t *tmp = firmwares->value; tmp != NULL; tmp = tmp->next) {
        jsmntok_t *ios = objectForKey(tmp, firmwarejson, (versVals->isBuildid) ? "buildid" : "version");
        if (ios->value->end - ios->value->start == strlen(versVals->version) && strncmp(versVals->version, firmwarejson + ios->value->start, strlen(versVals->version)) == 0) {
            
            if (versVals->isOta) {
                jsmntok_t *releaseType = NULL;
                if (versVals->useBeta && !(releaseType = objectForKey(tmp, firmwarejson, "releasetype"))) continue;
                else if (!versVals->useBeta);
                else if (strncmp(firmwarejson + releaseType->value->start, "Beta", releaseType->value->end - releaseType->value->start) != 0) continue;
            }
            
            jsmntok_t *url = objectForKey(tmp, firmwarejson, "url")->value;
            
            char *ret = malloc(url->end - url->start+1);
            char *cpy = ret;
            memset(ret, 0, url->end - url->start+1);
            for (int i=url->start ; i< url->end; i++) *(cpy++) = firmwarejson[i];
            
            
            jsmntok_t *i_vers = objectForKey(tmp, firmwarejson, "version");
            jsmntok_t *i_build = objectForKey(tmp, firmwarejson, "buildid");
            info("[TSSC] got firmwareurl for iOS %.*s build %.*s\n",i_vers->value->end - i_vers->value->start,firmwarejson + i_vers->value->start,i_build->value->end - i_build->value->start,firmwarejson + i_build->value->start);
            return ret;
        }
        
    }
    return NULL;
}

static void printline(int percent){
    info("%03d [",percent);for (int i=0; i<100; i++) putchar((percent >0) ? ((--percent > 0) ? '=' : '>') : ' ');
    info("]");
}

static void fragmentzip_callback(unsigned int progress){
    info("\x1b[A\033[J"); //clear 2 lines
    printline((int)progress);
    info("\n");
}

int downloadPartialzip(const char *url, const char *file, const char *dst){
    log("[LFZP] downloading %s from %s\n\n",file,url);
    fragmentzip_t *info = fragmentzip_open(url);
    if (!info) {
        error("[LFZP] failed to open url\n");
        return -1;
    }
    int ret = fragmentzip_download_file(info, file, dst, fragmentzip_callback);
    if (ret){
        error("[LFZP] failed to download file\n");
    }
    fragmentzip_close(info);
    return ret;
}

char *getBuildManifest(char *url, const char *device, const char *version, int isOta){
    struct stat st = {0};
    
    size_t len = strlen(MANIFEST_SAVE_PATH) + strlen("/_") + strlen(device) + strlen(version) +1;
    if (isOta) len += strlen("ota");
    char *fileDir = malloc(len);
    memset(fileDir, 0, len);
    
    strncat(fileDir, MANIFEST_SAVE_PATH, strlen(MANIFEST_SAVE_PATH));
    strncat(fileDir, DIRECTORY_DELIMITER_STR, 1);
    strncat(fileDir, device, strlen(device));
    strncat(fileDir, "_", strlen("_"));
    strncat(fileDir, version, strlen(version));
    
    if (isOta) strncat(fileDir, "ota", strlen("ota"));
    
    memset(&st, 0, sizeof(st));
    if (stat(MANIFEST_SAVE_PATH, &st) == -1) __mkdir(MANIFEST_SAVE_PATH, 0700);
    
    //gen name
    char *name = fileDir + strlen(fileDir);
    while (*--name != DIRECTORY_DELIMITER_CHR);
    name ++;
    
    //get file
    FILE *f = fopen(fileDir, "rb");
    if (!url) {
        if (!f || nocache) return NULL;
        info("[TSSC] using cached Buildmanifest for %s\n",name);
    }else info("[TSSC] opening Buildmanifest for %s\n",name);
    
    
    if (!f || nocache){
        //download if it isn't there
        if (downloadPartialzip(url, (isOta) ? "AssetData/boot/BuildManifest.plist" : "BuildManifest.plist", fileDir)){
            free(fileDir);
            return NULL;
        }
        f = fopen(fileDir, "rb");
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    //load it
    char *buildmanifest = malloc(fsize + 1);
    fread(buildmanifest, fsize, 1, f);
    buildmanifest[fsize] = '\0';
    fclose(f);
    
    
    free(fileDir);
    return buildmanifest;
}

int64_t getBBGCIDForDevice(const char *deviceModel){
    int64_t bbgcid = 0;
#define reterror(a ... ) {error(a); bbgcid = -1; goto error;}
    
    char *myjson = getBBCIDJson();
    
    jsmntok_t *tokens = NULL;
    int cnt = parseTokens(myjson, &tokens);
    if (cnt < 1) reterror("[TSSC] parsing bbgcid.json failed!\n");
    
    
    jsmntok_t *device = objectForKey(tokens, myjson, deviceModel);
    if (!device) {
        reterror("[TSSC] ERROR: device \"%s\" is not in bbgcid.json, which means it's BasebandGoldCertID isn't documented yet.\nIf you own such a device please consider contacting @tihmstar (tihmstar@gmail.com) to get instructions how to contribute to this project.\n",deviceModel);
    }
    if (device->type == JSMN_PRIMITIVE) {
        warning("[TSSC] WARNING: A BasebandGoldCertID is not required for %s\n",deviceModel);
        bbgcid = 0;
    }else{
        device = device->value;
        char * buf = malloc(device->end - device->size +1);
        strncpy(buf, myjson+device->start,device->end - device->size);
        buf[device->end - device->size] = 0;
        bbgcid = atoll(buf);
        free(buf);
    }
    
error:
    if (myjson) free(myjson);
    if (tokens) free(tokens);
    return bbgcid;
#undef reterror
}

void debug_plist(plist_t plist);

void getRandNum(char *dst, size_t size, int base){
    srand((unsigned int)time(NULL));
    for (int i=0; i<size; i++) {
        int j;
        if (base == 256) dst[i] = rand() % base;
        else dst[i] = ((j = rand() % base) < 10) ? '0' + j : 'a' + j-10;
    }
}

#pragma mark tss functions

int tss_populate_devicevals(plist_t tssreq, uint64_t ecid, char *nonce, size_t nonce_size, char *sep_nonce, size_t sep_nonce_size, int image4supported){
    
    plist_dict_set_item(tssreq, "ApECID", plist_new_uint(ecid)); //0000000000000000
    if (nonce) {
        plist_dict_set_item(tssreq, "ApNonce", plist_new_data(nonce, nonce_size));//aa aa aa aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa
    }
    
    if (sep_nonce) {//aa aa aa aa bb cc dd ee ff 00 11 22 33 44 55 66 77 88 99 aa
        plist_dict_set_item(tssreq, "ApSepNonce", plist_new_data(sep_nonce, sep_nonce_size));
    }
    
    plist_dict_set_item(tssreq, "ApProductionMode", plist_new_bool(1));
    
    if (image4supported) {
        plist_dict_set_item(tssreq, "ApSecurityMode", plist_new_bool(1));
        plist_dict_set_item(tssreq, "ApSupportsImg4", plist_new_bool(1));
    } else {
        plist_dict_set_item(tssreq, "ApSupportsImg4", plist_new_bool(0));
    }
    
    return 0;
}

int tss_populate_basebandvals(plist_t tssreq, plist_t tssparameters, int64_t BbGoldCertId){
    plist_t parameters = plist_new_dict();
    char bbnonce[NONCELEN_BASEBAND+1];
    char bbsnum[5];
    int64_t BbChipID = 0;
    
    getRandNum(bbnonce, NONCELEN_BASEBAND, 256);
    getRandNum(bbsnum, 4, 256);
    srand((unsigned int)time(NULL));
    int n=0; for (int i=1; i<7; i++) BbChipID += (rand() % 10) * pow(10, ++n);
    
    /* BasebandNonce not required */
//    plist_dict_set_item(parameters, "BbNonce", plist_new_data(bbnonce, noncelen));
    plist_dict_set_item(parameters, "BbChipID", plist_new_uint(BbChipID));
    plist_dict_set_item(parameters, "BbGoldCertId", plist_new_uint(BbGoldCertId));
    plist_dict_set_item(parameters, "BbSNUM", plist_new_data(bbsnum, 4));
    
    /* BasebandFirmware */
    plist_t BasebandFirmware = plist_access_path(tssparameters, 2, "Manifest", "BasebandFirmware");
    if (!BasebandFirmware || plist_get_node_type(BasebandFirmware) != PLIST_DICT) {
        error("ERROR: Unable to get BasebandFirmware node\n");
        return -1;
    }
    plist_t bbfwdict = plist_copy(BasebandFirmware);
    BasebandFirmware = NULL;
    if (plist_dict_get_item(bbfwdict, "Info")) {
        plist_dict_remove_item(bbfwdict, "Info");
    }
    plist_dict_set_item(tssreq, "BasebandFirmware", bbfwdict);
    
    tss_request_add_baseband_tags(tssreq, parameters, NULL);
    return 0;
}

int tss_populate_random(plist_t tssreq, int is64bit, t_devicevals *devVals){
    size_t nonceLen = 20; //valid for all devices up to iPhone7
    if (!devVals->deviceModel)
        return error("[TSSR] internal error: devVals->deviceModel is missing\n"),-1;
    
    if (strncmp(devVals->deviceModel, "iPhone9,", strlen("iPhone9,")) == 0)
        nonceLen = 32;
    
    
    char *nonce = (char*)malloc(nonceLen+1);
    char sep_nonce[NONCELEN_SEP+1];
    
    int n=0;
    srand((unsigned int)time(NULL));
    if (!devVals->ecid) for (int i=0; i<16; i++) devVals->ecid += (rand() % 10) * pow(10, n++);

    if (devVals->apnonce){
        if (devVals->parsedApnonceLen == nonceLen)
            memcpy(nonce, devVals->apnonce, nonceLen+1);
        else
            return error("[TSSR] parsed APNoncelen != requiredAPNoncelen (%u != %u)\n",(unsigned int)devVals->parsedApnonceLen,(unsigned int)nonceLen),-1;
    }else {
        if (nonceLen == 20) {
            //this is a pre iPhone7 device
            //nonce is derived from generator with SHA1
            unsigned char zz[8];
            getRandNum((char*)zz, 8, 256);
            
            snprintf(devVals->generator, 19, "0x%02x%02x%02x%02x%02x%02x%02x%02x",zz[7],zz[6],zz[5],zz[4],zz[3],zz[2],zz[1],zz[0]);
            SHA1(zz, 8, (unsigned char*)nonce);
        }else if (nonceLen == 32){
            //this is an iPhone7 device
            //nonce is derived from generator with ????
            error("iPhone7 device detected! Automatic generator->nonce calculation failed. Please manually specify an apnonce with len=%u\n",(unsigned int)nonceLen);
#warning TODO implement iPhone7 generator->nonce algorithm
            return -1;
        }else{
            return error("[TSSR] Automatic generator->nonce calculation failed. Unknown device with noncelen=%u\n",(unsigned int)nonceLen),-1;
        }
    }
    
    if (devVals->sepnonce){
        if (devVals->parsedApnonceLen == NONCELEN_SEP)
            memcpy(sep_nonce, devVals->sepnonce, NONCELEN_SEP+1);
        else
            return error("[TSSR] parsed SEPNoncelen != requiredSEPNoncelen (%u != %u)",(unsigned int)devVals->parsedSepnonceLen,(unsigned int)NONCELEN_SEP),-1;
    }else
        getRandNum(sep_nonce, NONCELEN_SEP, 256);
    
    nonce[nonceLen] = '\0';
    sep_nonce[NONCELEN_SEP] = '\0';
    
    debug("[TSSR] ecid=%llu\n",devVals->ecid);
    debug("[TSSR] nonce=%s\n",nonce);
    debug("[TSSR] sepnonce=%s\n",sep_nonce);
    
    int rt = tss_populate_devicevals(tssreq, devVals->ecid, nonce, nonceLen, sep_nonce, NONCELEN_SEP, is64bit);
    free(nonce);
    return rt;
}



int tssrequest(plist_t *tssrequest, char *buildManifest, t_devicevals *devVals, t_basebandMode basebandMode){
#define reterror(a) {error(a); error = -1; goto error;}
    int error = 0;
    plist_t manifest = NULL;
    plist_t tssparameter = plist_new_dict();
    plist_t tssreq = tss_request_new(NULL);
    
    plist_from_xml(buildManifest, (unsigned)strlen(buildManifest), &manifest);
    
    
    plist_t id0 = (devVals->deviceBoard)
                ? getBuildidentityWithBoardconfig(manifest, devVals->deviceBoard, devVals->isUpgradeInstall)
                : getBuildidentity(manifest, devVals->deviceModel, devVals->isUpgradeInstall);
    
    
    if (!id0 || plist_get_node_type(id0) != PLIST_DICT){
        reterror("[TSSR] Error: could not get id0\n");
    }
    plist_t manifestdict = plist_dict_get_item(id0, "Manifest");
    if (!manifestdict || plist_get_node_type(manifestdict) != PLIST_DICT){
        reterror("[TSSR] Error: could not get manifest\n");
    }
    plist_t sep = plist_dict_get_item(manifestdict, "SEP");
    int is64Bit = !(!sep || plist_get_node_type(sep) != PLIST_DICT);
    
    if (tss_populate_random(tssparameter,is64Bit,devVals))
        reterror("[TSSR] failed to populate tss request\n");
    
    tss_parameters_add_from_manifest(tssparameter, id0);
    if (tss_request_add_common_tags(tssreq, tssparameter, NULL) < 0) {
        reterror("[TSSR] ERROR: Unable to add common tags to TSS request\n");
    }
    
    if (tss_request_add_ap_tags(tssreq, tssparameter, NULL) < 0) {
        reterror("[TSSR] ERROR: Unable to add common tags to TSS request\n");
    }
    
    if (is64Bit) {
        if (tss_request_add_ap_img4_tags(tssreq, tssparameter) < 0) {
            reterror("[TSSR] ERROR: Unable to add img4 tags to TSS request\n");
        }
    } else {
        if (tss_request_add_ap_img3_tags(tssreq, tssparameter) < 0) {
            reterror("[TSSR] ERROR: Unable to add img3 tags to TSS request\n");
        }
    }
    if (basebandMode == kBasebandModeOnlyBaseband) {
        if (plist_dict_get_item(tssreq, "@ApImg4Ticket"))
            plist_dict_set_item(tssreq, "@ApImg4Ticket", plist_new_bool(0));
        if (plist_dict_get_item(tssreq, "@APTicket"))
            plist_dict_set_item(tssreq, "@APTicket", plist_new_bool(0));
        //TODO don't use .shsh2 ending and don't save generator when saving only baseband
        info("[TSSR] User specified to request only a Baseband ticket.\n");
    }
    
    if (basebandMode != kBasebandModeWithoutBaseband) {
        //TODO: verify that this being int64_t instead of uint64_t doesn't actually break something
        
        int64_t BbGoldCertId = (devVals->bbgcid) ? devVals->bbgcid : getBBGCIDForDevice(devVals->deviceModel);
        if (BbGoldCertId == -1) {
            if (basebandMode == kBasebandModeOnlyBaseband){
                reterror("[TSSR] failed to get BasebandGoldCertID, but requested to get only baseband ticket. Aborting here!\n");
            }
            warning("[TSSR] WARNING: there was an error getting BasebandGoldCertID, continuing without requesting Baseband ticket\n");
        }else if (BbGoldCertId) {
            tss_populate_basebandvals(tssreq,tssparameter,BbGoldCertId);
            tss_request_add_baseband_tags(tssreq, tssparameter, NULL);
        }else{
            log("[TSSR] LOG: device %s doesn't need a Baseband ticket, continuing without requesting a Baseband ticket\n",devVals->deviceModel);
        }
    }else{
        info("[TSSR] User specified not to request a Baseband ticket.\n");
    }
    
    *tssrequest = tssreq;
error:
    if (manifest) plist_free(manifest);
    if (tssparameter) plist_free(tssparameter);
    if (error) plist_free(tssreq), *tssrequest = NULL;
    return error;
#undef reterror
}

int isManifestBufSignedForDevice(char *buildManifestBuffer, t_devicevals *devVals, t_basebandMode basebandMode){
    int isSigned = 0;
    plist_t tssreq = NULL;
    plist_t apticket = NULL;
    
    if (tssrequest(&tssreq, buildManifestBuffer, devVals, basebandMode)){
        error("[TSSR] faild to build tssrequest\n");
        goto error;
    }
    isSigned = ((apticket = tss_request_send(tssreq, NULL)) > 0);

    
    if (print_tss_response) debug_plist(apticket);
    if (isSigned && save_shshblobs){
        plist_t manifest = 0;
        plist_from_xml(buildManifestBuffer, (unsigned)strlen(buildManifestBuffer), &manifest);
        plist_t build = plist_dict_get_item(manifest, "ProductBuildVersion");
        char *cbuild = 0;
        plist_get_string_val(build, &cbuild);
        plist_t pvers = plist_dict_get_item(manifest, "ProductVersion");
        char *cpvers = 0;
        plist_get_string_val(pvers, &cpvers);
        
        plist_t pecid = plist_dict_get_item(tssreq, "ApECID");
        plist_get_uint_val(pecid, &devVals->ecid);
        char *cecid = ecid_to_string(devVals->ecid);
        
        
        uint32_t size = 0;
        char* data = NULL;
        if (*devVals->generator)
            plist_dict_set_item(apticket, "generator", plist_new_string(devVals->generator));
        plist_to_xml(apticket, &data, &size);
        
        
        size_t tmpDeviceNameSize = strlen(devVals->deviceModel) + strlen("_") + 1;
        if (devVals->deviceBoard) tmpDeviceNameSize += strlen(devVals->deviceBoard);
        
        char *tmpDevicename = (char *)malloc(tmpDeviceNameSize);
        memset(tmpDevicename, 0, tmpDeviceNameSize);
        snprintf(tmpDevicename, tmpDeviceNameSize, "%s", devVals->deviceModel);
        if (devVals->deviceBoard) snprintf(tmpDevicename+strlen(tmpDevicename), tmpDeviceNameSize-strlen(tmpDevicename), "_%s",devVals->deviceBoard);
        
        size_t fnamelen = strlen(shshSavePath) + 1 + strlen(cecid) + tmpDeviceNameSize + strlen(cpvers) + strlen(cbuild) + strlen(DIRECTORY_DELIMITER_STR"__-.shsh2") + 1;
        char *fname = malloc(fnamelen);
        memset(fname, 0, fnamelen);
        size_t prePathLen= strlen(shshSavePath);
        if (shshSavePath[prePathLen-1] == DIRECTORY_DELIMITER_CHR) prePathLen--;
        strncpy(fname, shshSavePath, prePathLen);
        
        
        snprintf(fname+prePathLen, fnamelen, DIRECTORY_DELIMITER_STR"%s_%s_%s-%s.shsh%s",cecid,tmpDevicename,cpvers,cbuild, (*devVals->generator) ? "2" : "");
        
        
        FILE *shshfile = fopen(fname, "w");
        if (!shshfile) error("[Error] can't save shsh at %s\n",fname);
        else{
            fwrite(data, strlen(data), 1, shshfile);
            fclose(shshfile);
            info("Saved shsh blobs!\n");
        }
        
        free(tmpDevicename);
        plist_free(manifest);
        free(fname);
        free(cpvers);
        free(cbuild);
        free(data);
    }
    
error:
    if (tssreq) plist_free(tssreq);
    if (apticket) plist_free(apticket);
    return isSigned;
}

int isManifestSignedForDevice(const char *buildManifestPath, t_devicevals *devVals, t_iosVersion *versVals){
    int isSigned = 0;
#define reterror(a ...) {error(a); isSigned = -1; goto error;}
    plist_t manifest = NULL;
    plist_t ProductVersion = NULL;
    plist_t SupportedProductTypes = NULL;
    plist_t mDevice = NULL;
    char *bufManifest = NULL;
    
    info("[TSSC] opening %s\n",buildManifestPath);
    //filehandling
    FILE *fmanifest = fopen(buildManifestPath, "r");
    if (!fmanifest) reterror("[TSSC] ERROR: file %s nof found!\n",buildManifestPath);
    fseek(fmanifest, 0, SEEK_END);
    long fsize = ftell(fmanifest);
    fseek(fmanifest, 0, SEEK_SET);
    bufManifest = (char*)malloc(fsize + 1);
    bufManifest[fsize] = '\0';
    fread(bufManifest, fsize, 1, fmanifest);
    fclose(fmanifest);
    
    plist_from_xml(bufManifest, (unsigned)strlen(bufManifest), &manifest);
    if (!manifest)
        reterror("[TSSC] failed to load manifest\n");
    
    if (!versVals->version){
        ProductVersion = plist_dict_get_item(manifest, "ProductVersion");
        if (versVals->isBuildid) //TODO is this really an issue?
            reterror("[TSSC] Error, this option is not supported with buildid. Please use -i instead\n");
        plist_get_string_val(ProductVersion, (char**)&versVals->version);
    }
    if (!devVals->deviceModel)
        reterror("[TSSC] can't proceed without device info\n");
    
    SupportedProductTypes = plist_dict_get_item(manifest, "SupportedProductTypes");
    if (SupportedProductTypes) {
        for (int i=0; i<plist_array_get_size(SupportedProductTypes); i++) {
            mDevice = plist_array_get_item(SupportedProductTypes, i);
            char *ldevice = NULL;
            plist_get_string_val(mDevice, &ldevice);
            if (strncmp(ldevice, devVals->deviceModel, strlen(devVals->deviceModel)) == 0)
                goto checkedDeviceModel;
        }
    }
    
    reterror("[TSSC] selected device can't be used with that buildmanifest\n");
    
checkedDeviceModel:
    isSigned = isManifestBufSignedForDevice(bufManifest, devVals, versVals->basebandMode);
    
error:
    if (manifest) plist_free(manifest);
    if (bufManifest) free(bufManifest);
    return isSigned;
#undef reterror
}

int isVersionSignedForDevice(char *firmwareJson, jsmntok_t *firmwareTokens, t_iosVersion *versVals, t_devicevals *devVals){
#define reterror(a ... ) {error(a); goto error;}
    if (atoi(versVals->version) <= 3) {
        info("[TSSC] WARNING: version to check \"%s\" seems to be iOS 3 or lower, which did not require SHSH or APTicket.\n\tSkipping checks and returning true.\n",versVals->version);
        return 1;
    }
    
    int isSigned = 0;
    char *url = NULL;
    char *buildManifest = NULL;
    
    if (!(buildManifest = getBuildManifest(NULL, devVals->deviceModel, versVals->version, versVals->isOta))){
        
        
        if (!checkFirmwareForDeviceExists(devVals, versVals, firmwareJson, firmwareTokens))
            return error("[TSSC] ERROR: either device %s does not exist, or there is no iOS %s for it.\n",devVals->deviceModel,versVals->version), 0;
        
        
        url = getFirmwareUrl(devVals, versVals, firmwareJson, firmwareTokens);
        if (!url) reterror("[TSSC] ERROR: could not get url for device %s on iOS %s\n",devVals->deviceModel,versVals->version);
        buildManifest = getBuildManifest(url, devVals->deviceModel, versVals->version, versVals->isOta);
        if (!buildManifest) reterror("[TSSC] ERROR: could not get BuildManifest for firmwareurl %s\n",url);
    }
    
    isSigned = isManifestBufSignedForDevice(buildManifest, devVals, versVals->basebandMode);

error:
    if (url) free(url);
    if (buildManifest) free(buildManifest);
    return isSigned;
#undef reterror
}

#pragma mark print functions

#warning print devices function doesn't actually check if devices are sorted. it assues they are sorted in json
int printListOfDevices(char *firmwarejson, jsmntok_t *tokens){
#define MAX_PER_LINE 10
    log("[JSON] printing device list\n");
    int curr = 0;
    int currLen = 0;
    int rspn = 0;
    putchar('\n');
    jsmntok_t *ctok = objectForKey(tokens, firmwarejson, "devices");
    for (jsmntok_t *tmp = ctok->value; ; tmp = tmp->next) {
        if (!curr){
            curr = tmp->start;
            currLen = tmp->end - tmp->start;
        }else{
            for (int i = 0; i<currLen; i++) {
                char c = *(firmwarejson+curr+i);
                if (!(('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z'))) break;
                if (c != *(firmwarejson+tmp->start+i)) {
                    putchar('\n');
                    putchar('\n');
                    curr = tmp->start;
                    currLen = tmp->end - tmp->start;
                    rspn = 0;
                    break;
                }
            }
        }
        for (int j=0; j<tmp->end-tmp->start; j++) {
            putchar(*(firmwarejson+tmp->start + j));
        }
        if (++rspn>= MAX_PER_LINE) putchar('\n'), rspn = 0; else putchar(' ');
        
        if (tmp->next == ctok->value) break;
    }
    putchar('\n');
    putchar('\n');
    return 0;
#undef MAX_PER_LINE
}

int cmpfunc(const void * a, const void * b){
    char *aa = *(char**)a;
    char *bb = *(char**)b;
    int d;
    
    while(1) {
        d = atoi(bb) - atoi(aa);
        if(d != 0)
            return d;
        aa = strchr(aa, '.');
        bb = strchr(bb, '.');
        if(aa == NULL || bb == NULL) {
            if(aa == NULL && bb == NULL) {
                aa = strchr(*(char**)a, '[');
                bb = strchr(*(char**)b, '[');
                return aa == NULL ? (bb == NULL ? 0 : -1) : 1;
            }
            return aa == NULL ? 1 : -1;
        }
        aa++;
        bb++;
    }
}

char **getListOfiOSForDevice(char *firmwarejson, jsmntok_t *tokens, const char *device, int isOTA, int *versionCntt){
    //requires free(versions[versionsCnt-1]); and free(versions); after use
    jsmntok_t *firmwares = getFirmwaresForDevice(device, firmwarejson, tokens, isOTA);
    
    if (!firmwares)
        return error("[TSSC] device %s could not be found in devicelist\n",device),NULL;
    
    int versionsCnt = firmwares->size;
    char **versions = (char**)malloc(versionsCnt * sizeof(char *));
    
    
    for (jsmntok_t *tmp = firmwares->value; tmp != NULL; tmp = tmp->next) {
        
        jsmntok_t *ios = objectForKey(tmp, firmwarejson, "version");
        
        int verslen= ios->value->end-ios->value->start;
        int isBeta = 0;
        jsmntok_t *releaseType = NULL;
        if ((releaseType = objectForKey(tmp, firmwarejson, "releasetype"))) {
            isBeta = (strncmp(firmwarejson + releaseType->value->start, "Beta", releaseType->value->end - releaseType->value->start) == 0);
        }
        
        versions[--versionsCnt] = (char*)malloc((verslen+1 + isBeta * strlen("[B]")) * sizeof(char));
        strncpy(versions[versionsCnt], firmwarejson + ios->value->start, verslen);
        if (isBeta) strncpy(&versions[versionsCnt][verslen], "[B]", strlen("[B]"));
        versions[versionsCnt][verslen + isBeta * strlen("[B]")] = '\0';
    }
    versionsCnt = firmwares->size;
    qsort(versions, versionsCnt, sizeof(char *), &cmpfunc);
    if (versionCntt) *versionCntt = versionsCnt;
    return versions;
}


int printListOfiOSForDevice(char *firmwarejson, jsmntok_t *tokens, char *device, int isOTA){
#define MAX_PER_LINE 10
    
    int versionsCnt;
    char **versions = getListOfiOSForDevice(firmwarejson, tokens, device, isOTA, &versionsCnt);
    
    int rspn = 0,
        currVer = 0,
        nextVer = 0;
    for (int i=0; i<versionsCnt; i++) {
        if (i){
            int res = strcmp(versions[i-1], versions[i]);
            free(versions[i-1]);
            if (res == 0) continue;
        }
        
        
        nextVer = atoi(versions[i]);
        if (currVer && currVer != nextVer) printf("\n"), rspn = 0;
        currVer = nextVer;
        if (!rspn) printf("[iOS %2i] ",currVer);
        int printed = 0;
        printf("%s%n",versions[i],&printed);
        while (printed++ < 12) putchar(' ');
        if (++rspn>= MAX_PER_LINE) putchar('\n'), rspn = 0; else putchar(' ');
    }
    free(versions[versionsCnt-1]);
    free(versions);
    
    printf("\n\n");
    return 0;
#undef MAX_PER_LINE
}


#pragma mark check functions

jsmntok_t *getFirmwaresForDevice(const char *device, const char *firmwareJson, jsmntok_t *tokens, int isOta){
    jsmntok_t *ctok = (isOta) ? tokens : objectForKey(tokens, firmwareJson, "devices");
    for (jsmntok_t *tmp = ctok->value; ; tmp = tmp->next) {
        
        if (strncmp(device, firmwareJson+tmp->start, tmp->end - tmp->start) == 0)
            return objectForKey(tmp, firmwareJson, "firmwares");
        
        if (tmp->next == ctok->value) break;
    }
    
    return NULL;
}

int checkFirmwareForDeviceExists(t_devicevals *devVals, t_iosVersion *versVals, char *firmwareJson, jsmntok_t *tokens){
    
    jsmntok_t *firmwares = getFirmwaresForDevice(devVals->deviceModel, firmwareJson, tokens, versVals->isOta);
    if (!firmwares)
        return 0;
    
    for (jsmntok_t *tt = firmwares->value; tt ;tt = tt->next) {
        
        jsmntok_t *versiont = objectForKey(tt, firmwareJson, (versVals->isBuildid) ? "buildid" : "version");
        size_t len = versiont->value->end - versiont->value->start;
        if (len == strlen(versVals->version) && strncmp(versVals->version, firmwareJson + versiont->value->start, len) == 0) return 1;
    }
    
    return 0;
}
