#include "webserver.hpp"

static const char *TAG = "webserver";
int led_state = 0;
httpd_handle_t server = NULL;
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

#define INDEX_HTML_PATH "/spiffs/index.html"
char index_html[20000];
char response_data[20000];

void setDrivers(cJSON *driversJson, bool readNVS)
{
    printf("Setting drivers\n\r");
    if(readNVS)
    {
        printf("Reading NVS for drivers DC\n\r");
        nvs_load_drivers(driversJson);
        printf("%s",cJSON_Print(driversJson));
    }
    else nvs_save_drivers(driversJson);

    cJSON *first_driver_dc_obj = cJSON_GetObjectItem(cJSON_GetObjectItem(driversJson, "firstDriver"), "dutyCycle");
    cJSON *second_driver_dc_obj = cJSON_GetObjectItem(cJSON_GetObjectItem(driversJson, "secondDriver"), "dutyCycle");
    cJSON *third_driver_dc_obj = cJSON_GetObjectItem(cJSON_GetObjectItem(driversJson, "thirdDriver"), "dutyCycle");
    cJSON *fourth_driver_dc_obj = cJSON_GetObjectItem(cJSON_GetObjectItem(driversJson, "fourthDriver"), "dutyCycle");
    const uint8_t first_driver_dc = strtoul(first_driver_dc_obj->valuestring, NULL, 10);
    const uint8_t second_driver_dc = strtoul(second_driver_dc_obj->valuestring, NULL, 10);
    const uint8_t third_driver_dc = strtoul(third_driver_dc_obj->valuestring, NULL, 10);
    const uint8_t fourth_driver_dc = strtoul(fourth_driver_dc_obj->valuestring, NULL, 10);

    printf("Setting drivers DC1: %d, DC2: %d, DC3:%d, DC4: %d\n\r", first_driver_dc, second_driver_dc, third_driver_dc, fourth_driver_dc);

    LEDDrivers[0].setDuty(first_driver_dc);
    LEDDrivers[1].setDuty(second_driver_dc);
    LEDDrivers[2].setDuty(third_driver_dc);
    LEDDrivers[3].setDuty(fourth_driver_dc);
    printf("Drivers Set\n\r");
}

// Read spiff and place index.html in buffer index_html
static void initi_web_page_buffer(void)
{
    printf("In function \"initi_web_page_buffer\": Read spiff and place index.html in buffer index_html\n\r");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true};

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

    memset((void *)index_html, 0, sizeof(index_html));
    struct stat st;
    if (stat(INDEX_HTML_PATH, &st))
    {
        ESP_LOGE(TAG, "index.html not found");
        return;
    }

    FILE *fp = fopen(INDEX_HTML_PATH, "r");
    if (fread(index_html, st.st_size, 1, fp) == 0)
    {
        ESP_LOGE(TAG, "fread failed");
    }
    fclose(fp);
}

// used to send a web page to the client in response to an HTTP request.
esp_err_t get_req_handler(httpd_req_t *req)
{
    cJSON *driversJson = cJSON_CreateObject();
    setDrivers(driversJson, 1);
    printf("%s\n\r", cJSON_Print(driversJson));
    return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

void send_json_to_all_clients(httpd_handle_t hd, cJSON *object)
{
    char* json_str = cJSON_Print(object);
    printf("json str: %s\n\r", json_str);
    httpd_ws_frame_t ws_pkt;
    ws_pkt.payload = (uint8_t *)json_str;
    ws_pkt.len = strlen(json_str);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    static size_t max_clients = CONFIG_LWIP_MAX_LISTENING_TCP;
    size_t fds = max_clients;
    int client_fds[max_clients];

    esp_err_t ret = httpd_get_client_list(server, &fds, client_fds);
    if (ret != ESP_OK) return;

    for (int i = 0; i < fds; i++) {
        int client_info = httpd_ws_get_fd_info(server, client_fds[i]);
        if (client_info == HTTPD_WS_CLIENT_WEBSOCKET) {
            httpd_ws_send_frame_async(hd, client_fds[i], &ws_pkt);
            printf("Sended to client\n\r");
        }
    }
    printf("JSON sended to all clients\n\r");
}

static esp_err_t handle_ws_req(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len)
    {
        buf = static_cast<uint8_t*>(calloc(1, ws_pkt.len + 1));
        if (buf == NULL)
        {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf; //set to same point as buf
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);

    // Parse JSON string into a cJSON object
    cJSON *object = cJSON_ParseWithLength(reinterpret_cast<const char*>(ws_pkt.payload), ws_pkt.len);

    if (object == nullptr) {
        printf("Failed to parse JSON string\n\r");
        return 1;
    }

    cJSON *action = cJSON_GetObjectItemCaseSensitive(object, "action");
    if (cJSON_IsString(action)) {
        char* actionValue = action->valuestring;
        printf("Action: %s\n\r", actionValue);
        if(strcmp(actionValue, "dutyCycle") == 0)
        {
            send_json_to_all_clients(req->handle, object);
            cJSON *driversJson = cJSON_GetObjectItem(object, "drivers");
            setDrivers(driversJson, 0);
        } 
        else if(strcmp(actionValue, "setTime") == 0)
        {
            cJSON *timeJson = cJSON_GetObjectItem(object, "setTime");
            nvs_save_time(timeJson);

            char on_time[6], off_time[6];
            nvs_load_on_time(on_time, sizeof(on_time)); 
            nvs_load_off_time(off_time, sizeof(off_time)); 
            printf("Loaded onTime: %s and offTime: %s to the NVS\n\r", on_time, off_time);
        }
        else printf("action does not exist\n\r");
    }
    else printf("action not found\n\r");

    cJSON_Delete(object);

    return ESP_OK;
}

httpd_handle_t setup_websocket_server(void)
{
    printf("In setup_websocket_server\r\n");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = get_req_handler,
        .user_ctx = NULL};

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handle_ws_req,
        .user_ctx = NULL,
        .is_websocket = true};

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &ws);
    }
    printf("server: %p\n\r", server);
    return server;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        printf("In connect handler\r\n");
        initi_web_page_buffer();
        *server = setup_websocket_server();
    }
}