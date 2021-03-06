/************************************************************************************************************************
 Copyright (c) 2016, Imagination Technologies Limited and/or its affiliated group companies.
 All rights reserved.
 Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 following conditions are met:
     1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
        following disclaimer.
     2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
        following disclaimer in the documentation and/or other materials provided with the distribution.
     3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
        products derived from this software without specific prior written permission.
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************************************************/

#include "ubus_agent.h"

#include <stdint.h>
#include <stdbool.h>
#include <libubus.h>
#include <libubox/blobmsg_json.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <semaphore.h>
#include <sys/prctl.h>
#include <glib.h>

#include "clicker.h"
#include "controls.h"
#include "provision_history.h"
#include "utils.h"
#include "commands.h"

//forward declarations
static int GetStateMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg);

static int SelectMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg);

static int StartProvisionMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg);

static int SetClickerNameMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg);

//variables & structs
typedef struct {
    int clickerId;
    bool toDelete;
    struct ubus_request request;
    struct blob_buf replyBloob;
} UBusMemoryBlock;

static struct ubus_context *_UbusCTX;
static pthread_t _UbusThread;
static char *_Path;
static GMutex _Mutex;
static bool _UBusRunning = false;
static bool _UBusInterruption = false;
static bool _UBusInInterState = false;
struct uloop_timeout _UBusHelperProcess;
//holds UBusMemoryBlock elements
GSList* _UBusRequestMemory = NULL;

static const struct blobmsg_policy _GetStatePolicy[] = {
};


enum {
    SELECT_CLICKER_ID,

    SELECT_LAST_ENUM
};

static const struct blobmsg_policy _SelectPolicy[] = {
    [SELECT_CLICKER_ID] = { .name = "clickerID", .type = BLOBMSG_TYPE_INT32 },
};

static const struct blobmsg_policy _StartProvisionPolicy[] = {
    [SELECT_CLICKER_ID] = { .name = "clickerID", .type = BLOBMSG_TYPE_INT32 },
};

enum {
     SET_CLICKER_NAME_CLICKER_ID,
     SET_CLICKER_NAME_CLICKER_NAME,

     SET_CLICKER_NAME_LAST
 };

 static const struct blobmsg_policy _SetClickerNamePolicy[] = {
     [SET_CLICKER_NAME_CLICKER_ID] = {.name = "clickerID", .type = BLOBMSG_TYPE_INT32 },
     [SET_CLICKER_NAME_CLICKER_NAME] = {.name = "clickerName", .type = BLOBMSG_TYPE_STRING }
 };

static const struct ubus_method _UBusAgentMethods[] = {
    UBUS_METHOD("getState", GetStateMethodHandler, _GetStatePolicy),
    UBUS_METHOD("select", SelectMethodHandler, _SelectPolicy),
    UBUS_METHOD("startProvision", StartProvisionMethodHandler, _StartProvisionPolicy),
    UBUS_METHOD("setClickerName", SetClickerNameMethodHandler, _SetClickerNamePolicy)
};

static struct ubus_object_type _UBusAgentObjectType = UBUS_OBJECT_TYPE("provisioning-daemon", _UBusAgentMethods);

static struct ubus_object _UBusAgentObject = {
    .name = "provisioning-daemon",
    .type = &_UBusAgentObjectType,
    .methods = _UBusAgentMethods,
    .n_methods = ARRAY_SIZE(_UBusAgentMethods),
};

enum {
    GENERATE_PSK_RESPONSE_ID,
    GENERATE_PSK_RESPONSE_PSK_IDENTITY,
    GENERATE_PSK_RESPONSE_PSK_SECRET,
    GENERATE_PSK_RESPONSE_ERROR,
    GENERATE_PSK_RESPONSE_MAX
};

static const struct blobmsg_policy _GeneratePskResponsePolicy[GENERATE_PSK_RESPONSE_MAX] =
{
    [GENERATE_PSK_RESPONSE_ID] = {.name = "id", .type = BLOBMSG_TYPE_INT32},
    [GENERATE_PSK_RESPONSE_PSK_IDENTITY] = {.name = "pskIdentity", .type = BLOBMSG_TYPE_STRING},
    [GENERATE_PSK_RESPONSE_PSK_SECRET] = {.name = "pskSecret", .type = BLOBMSG_TYPE_STRING},
    [GENERATE_PSK_RESPONSE_ERROR] = {.name = "error", .type = BLOBMSG_TYPE_STRING},
};

static UBusMemoryBlock* CreateUBusMemoryBlock() {
    //Note: Should be called from critical section
    UBusMemoryBlock* result = g_malloc0(sizeof(UBusMemoryBlock));
    _UBusRequestMemory = g_slist_prepend(_UBusRequestMemory, result);
    memset(&result->replyBloob, 0, sizeof(result->replyBloob));
    blob_buf_init(&result->replyBloob, 0);
    result->toDelete = false;
    return result;
}

static void ReleaseUBusMemoryBlock(UBusMemoryBlock* block) {
    //Note: Should be called from critical section
    _UBusRequestMemory = g_slist_remove(_UBusRequestMemory, block);
    blob_buf_free(&block->replyBloob);
    g_free(block);
}

static int SetClickerNameMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg) {

    g_debug("uBusAgent: Requested SetClickerName");

    struct blob_attr *args[SET_CLICKER_NAME_LAST];

    blobmsg_parse(_SetClickerNamePolicy, SET_CLICKER_NAME_LAST, args, blob_data(msg), blob_len(msg));

    int clickerID;
    if (args[SET_CLICKER_NAME_CLICKER_ID]) {
        clickerID = blobmsg_get_u32(args[SET_CLICKER_NAME_CLICKER_ID]);
    } else {
        return 1;
    }

    Clicker *clicker = clicker_AcquireOwnership(clickerID);
    if (clicker == NULL) {
        g_critical("uBusAgent: No clicker with id %d", clickerID);
        return UBUS_STATUS_NO_DATA;
    }

    char *clickerName;
    if (args[SET_CLICKER_NAME_CLICKER_NAME]) {
        clickerName = blobmsg_get_string(args[SET_CLICKER_NAME_CLICKER_NAME]);
    } else {
        clicker_ReleaseOwnership(clicker);
        return UBUS_STATUS_NO_DATA;
    }

    g_free(clicker->name);
    int size = strlen(clickerName);
    size = size > COMMAND_ENDPOINT_NAME_LENGTH ? COMMAND_ENDPOINT_NAME_LENGTH : size;
    clicker->name = g_malloc( size );
    strlcpy(clicker->name, clickerName, COMMAND_ENDPOINT_NAME_LENGTH);

    clicker_ReleaseOwnership(clicker);

    return UBUS_STATUS_OK;
}

static int SelectMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg)
{

    struct blob_attr* argBuffer[SELECT_LAST_ENUM];

    blobmsg_parse(_SelectPolicy, ARRAY_SIZE(_SelectPolicy), argBuffer, blob_data(msg), blob_len(msg));

    uint32_t clickerId = 0xffffffff;
    if (argBuffer[SELECT_CLICKER_ID]) {
        clickerId = blobmsg_get_u32(argBuffer[SELECT_CLICKER_ID]);
    } else {
        return UBUS_STATUS_NO_DATA;
    }

    g_info("uBusAgent: Select, move to clickerId:%d", clickerId);

    event_PushEventWithInt(EventType_CLICKER_SELECT, clickerId);

    return UBUS_STATUS_OK;
}

static int StartProvisionMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg)
{
    struct blob_attr* argBuffer[SELECT_LAST_ENUM];

    blobmsg_parse(_StartProvisionPolicy, ARRAY_SIZE(_StartProvisionPolicy), argBuffer, blob_data(msg), blob_len(msg));

    uint32_t clickerId = 0xffffffff;
    if (argBuffer[SELECT_CLICKER_ID]) {
        clickerId = blobmsg_get_u32(argBuffer[SELECT_CLICKER_ID]);
    } else {
        clickerId = controls_GetSelectedClickerId();
    }
    g_info("uBusAgent: Requested StartProvision, clicker id: %d", clickerId);
    event_PushEventWithInt(EventType_CLICKER_START_PROVISION, clickerId);

    return UBUS_STATUS_OK;
}

static int GetStateMethodHandler(struct ubus_context *ctx, struct ubus_object *obj, struct ubus_request_data *req,
        const char *method, struct blob_attr *msg)
{
    //g_debug("uBusAgent: Requested GetState");

    GArray* connectedClickers = controls_GetAllClickersIds();
    int selClickerId = controls_GetSelectedClickerId();
    //g_debug("uBusAgent: GetState clicker count:%d, selected id:%d", connectedClickers->len, selClickerId);

    //add history data
    GArray* historyItems = history_GetProvisioned();
    struct blob_buf replyBloob = {0, NULL, 0, NULL};
    blob_buf_init(&replyBloob, 0);
    void* cookie_array = blobmsg_open_array(&replyBloob, "clickers");
    for(int t = 0; t < historyItems->len; t++)
    {
        void* cookie_item = blobmsg_open_table(&replyBloob, "clicker");
        HistoryItem* history = &g_array_index(historyItems, HistoryItem, t);
        blobmsg_add_u32(&replyBloob, "id", history->id);
        blobmsg_add_string(&replyBloob, "name", history->name);
        blobmsg_add_u8(&replyBloob, "selected", false);
        blobmsg_add_u8(&replyBloob, "inProvisionState", false);
        blobmsg_add_u8(&replyBloob, "isProvisioned", true);
        blobmsg_add_u8(&replyBloob, "isError", history->isErrored);
        blobmsg_close_table(&replyBloob, cookie_item);
    }

    bool alreadyProvisioned = false;

    for(int t = 0; t < connectedClickers->len; t++)
    {
        Clicker* clk = clicker_AcquireOwnership( g_array_index(connectedClickers, int, t) );
        if (clk == NULL)
            continue;

        alreadyProvisioned = false;
        for(int j = 0; j < historyItems->len; j++)
        {
            HistoryItem* history = &g_array_index(historyItems, HistoryItem, j);
            if (history->id == clk->clickerID)
            {
                alreadyProvisioned = true;
                break;
            }
        }

        if (alreadyProvisioned)
        {
            clicker_ReleaseOwnership(clk);
            continue;
        }
        void* cookie_item = blobmsg_open_table(&replyBloob, "clicker");

        blobmsg_add_u32(&replyBloob, "id", clk->clickerID);
        blobmsg_add_string(&replyBloob, "name", clk->name);
        blobmsg_add_u8(&replyBloob, "selected", clk->clickerID == selClickerId);
        blobmsg_add_u8(&replyBloob, "inProvisionState", clk->provisioningInProgress);
        blobmsg_add_u8(&replyBloob, "isProvisioned", false);
        blobmsg_add_u8(&replyBloob, "isError", clk->error > 0 ? true : false);

        blobmsg_close_table(&replyBloob, cookie_item);

        clicker_ReleaseOwnership(clk);
    }
    g_array_free(historyItems, TRUE);
    g_array_free(connectedClickers, TRUE);
    blobmsg_close_array(&replyBloob, cookie_array);

    ubus_send_reply(ctx, req, replyBloob.head);
    blob_buf_free(&replyBloob);
    return UBUS_STATUS_OK;
}

static void GeneratePskResponseHandler(struct ubus_request *req, int type, struct blob_attr *msg)
{
    g_critical("Got: %p", msg);
    struct blob_attr *args[GENERATE_PSK_RESPONSE_MAX];

    blobmsg_parse(_GeneratePskResponsePolicy, GENERATE_PSK_RESPONSE_MAX, args, blob_data(msg), blob_len(msg));
    UBusMemoryBlock* block = (UBusMemoryBlock*)req->priv;

    char *error = NULL;
    if (args[GENERATE_PSK_RESPONSE_ERROR]) {
        error = blobmsg_get_string(args[GENERATE_PSK_RESPONSE_ERROR]);
    }

    if (error) {
        g_critical("uBusAgent: Error while generating PSK : %s", error);
        PreSharedKey* eventData = g_new0(PreSharedKey, 1);
        eventData->clickerId = block->clickerId;
        event_PushEventWithPtr(EventType_PSK_OBTAINED, eventData, true);
        block->toDelete = true;
        return;
    }

    char *psk = NULL;
    if (args[GENERATE_PSK_RESPONSE_PSK_SECRET]) {
        psk = blobmsg_get_string(args[GENERATE_PSK_RESPONSE_PSK_SECRET]);
    }

    if (!psk) {
        g_critical("uBusAgent: UNKNOWN PSK");
        PreSharedKey* eventData = g_new0(PreSharedKey, 1);
        eventData->clickerId = block->clickerId;
        event_PushEventWithPtr(EventType_PSK_OBTAINED, eventData, true);
        block->toDelete = true;
        return;
    }

    char *identity;
    if (args[GENERATE_PSK_RESPONSE_PSK_IDENTITY]) {
        identity = blobmsg_get_string(args[GENERATE_PSK_RESPONSE_PSK_IDENTITY]);
    }

    if (!identity) {
        g_critical("uBusAgent: UNKNOWN PSK");
        PreSharedKey* eventData = g_new0(PreSharedKey, 1);
        eventData->clickerId = block->clickerId;
        event_PushEventWithPtr(EventType_PSK_OBTAINED, eventData, true);
        block->toDelete = true;
        return;
    }

    g_message("uBusAgent: Obtained PSK: %s and IDENTITY: %s", psk, identity);

    PreSharedKey* eventData = g_new0(PreSharedKey, 1);
    eventData->clickerId = block->clickerId;

    strlcpy(eventData->identity, identity, PSK_ARRAYS_SIZE);
    eventData->identityLen = strlen(identity);
    eventData->identityLen = eventData->identityLen > PSK_ARRAYS_SIZE ? PSK_ARRAYS_SIZE : eventData->identityLen;

    strlcpy(eventData->psk, psk, PSK_ARRAYS_SIZE);
    eventData->pskLen = strlen(psk);
    eventData->pskLen = eventData->pskLen > PSK_ARRAYS_SIZE ? PSK_ARRAYS_SIZE : eventData->pskLen;

    event_PushEventWithPtr(EventType_PSK_OBTAINED, eventData, true);
    block->toDelete = true;
    return;
}

void HelperTimeoutHandler(struct uloop_timeout *t)
{
    g_mutex_lock(&_Mutex);
    if (_UBusInterruption)
        uloop_cancelled = true;

    g_mutex_unlock(&_Mutex);
    uloop_timeout_set(&_UBusHelperProcess, 500);
}

static void* PDUbusLoop(void *arg)
{
    g_message("uBusAgent: uBus thread started.\n");
    while(true)
    {
        g_mutex_lock(&_Mutex);
        if (_UBusRunning == false)
        {
            g_mutex_unlock(&_Mutex);
            break;
        }

        while(_UBusInterruption)
        {
            _UBusInInterState = true;
            g_debug("Interrupt state");
            g_mutex_unlock(&_Mutex);
            usleep(1000 * 1000);
            g_mutex_lock(&_Mutex);
            _UBusInInterState = false;
        }
        //check for memory removal
        if (_UBusRequestMemory != NULL) {
            UBusMemoryBlock* block = (UBusMemoryBlock*)_UBusRequestMemory->data;
            if ( block->toDelete ) {
                ReleaseUBusMemoryBlock(block);
            }
        }
        g_mutex_unlock(&_Mutex);
        uloop_run();
    }
    g_message("uBusAgent: uBus thread finish.\n");
    return NULL;
}

static void SetUBusRunning(bool state)
{
    g_mutex_lock(&_Mutex);
    _UBusRunning = state;
    g_mutex_unlock(&_Mutex);
}

static void SetUBusLoopInterruption(bool state)
{
    g_mutex_lock(&_Mutex);
    _UBusInterruption = state;
    uloop_cancelled = state;
    g_mutex_unlock(&_Mutex);
}

static void WaitForInterruptState(void)
{
    while(true)
    {
        g_mutex_lock(&_Mutex);
        bool ok = _UBusInInterState;
        g_mutex_unlock(&_Mutex);
        if (ok)
            break;

        usleep(200 * 1000);
    }
}

bool ubusagent_Init(void)
{
    g_mutex_init(&_Mutex);
    uloop_init();
    _UbusCTX = ubus_connect(_Path);
    if (!_UbusCTX)
    {
        g_critical("uBusAgent: Failed to connect to ubus");
        return false;
    }
    ubus_add_uloop(_UbusCTX);

    memset(&_UBusHelperProcess, 0, sizeof(_UBusHelperProcess));
    _UBusHelperProcess.cb = HelperTimeoutHandler;
    uloop_timeout_set(&_UBusHelperProcess, 500);

    SetUBusRunning(true);
    SetUBusLoopInterruption(false);
    _UBusInInterState = false;

    if (pthread_create(&_UbusThread, NULL, PDUbusLoop, NULL) < 0)
    {
        g_critical("uBusAgent: Error creating thread.");
        return false;
    }
    return true;
}

bool ubusagent_EnableRemoteControl(void)
{
    SetUBusLoopInterruption(true);
    WaitForInterruptState();
    g_message("uBusAgent: Enabling provision control through uBus");
    int ret = ubus_add_object(_UbusCTX, &_UBusAgentObject);
    if (ret)
        g_critical("uBusAgent: Failed to add object: %s\n", ubus_strerror(ret));

    SetUBusLoopInterruption(false);

    return ret == 0;
}

void ubusagent_Destroy(void)
{
    SetUBusLoopInterruption(false);
    SetUBusRunning(false);
    if (_UbusCTX)
        ubus_free(_UbusCTX);

    uloop_done();

    while (_UBusRequestMemory != NULL) {
        UBusMemoryBlock* block = (UBusMemoryBlock*) _UBusRequestMemory->data;
        if (block->toDelete) {
            ReleaseUBusMemoryBlock(block);
        }
    }
    g_mutex_clear(&_Mutex);
}

//Warn: this is blocking call
bool ubusagent_SendGeneratePskMessage(int clickerId)
{
    SetUBusLoopInterruption(true);
    WaitForInterruptState();
    uint32_t id, ret;
    ret = ubus_lookup_id(_UbusCTX, "creator", &id);
    if (ret)
    {
        g_critical("uBusAgent: creator ubus service not available");
        SetUBusLoopInterruption(false);
        return false;
    }

    g_mutex_lock(&_Mutex);
    UBusMemoryBlock* block = CreateUBusMemoryBlock();
    g_mutex_unlock(&_Mutex);
    block->clickerId = clickerId;
    ret = ubus_invoke_async(_UbusCTX, id, "generatePsk", block->replyBloob.head, &block->request);
    block->request.priv = (void*)block;
    block->request.data_cb = GeneratePskResponseHandler;
    ubus_complete_request_async(_UbusCTX, &block->request);

    if (ret)
    {
        g_critical("uBusAgent: Filed to invoke generatePsk");
        SetUBusLoopInterruption(false);
        return false;
    }
    SetUBusLoopInterruption(false);
    return true;
}
