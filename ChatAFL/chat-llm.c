#define _GNU_SOURCE // asprintf
#include <stdio.h>
#include <curl/curl.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "chat-llm.h"
#include "alloc-inl.h"
#include "hash.h"

// -lcurl -ljson-c -lpcre2-8
// apt install libcurl4-openssl-dev libjson-c-dev libpcre2-dev libpcre2-8-0

#define MAX_TOKENS 2048   //....................????????????????????/////?
#define CONFIDENT_TIMES 3

struct MemoryStruct
{
    char *memory;
    size_t size;
};

//contents: the response come from the LLM, size: the size of the data, nmemb: the number of the data, userp: the pointer to the struct MemoryStruct to store the data
static size_t chat_with_llm_helper(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb; //total size of the response
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL)
    {
        /* out of memory! */
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0; //ensure the string is terminated

    return realsize;
}

//get the "content" from the curl response
char *chat_with_llm(char *prompt, char *model, int tries, float temperature)
{
    CURL *curl;
    CURLcode res = CURLE_OK;
    char *answer = NULL;
    char *url = "https://api.openai.com/v1/chat/completions";
    char *auth_header = "Authorization: Bearer " OPENAI_TOKEN;
    char *content_header = "Content-Type: application/json";
    char *accept_header = "Accept: application/json";
    char *data = NULL;
    asprintf(&data, "{\"model\": \"gpt-4o-mini\",\"messages\": %s, \"max_tokens\": %d, \"temperature\": %f}", prompt, MAX_TOKENS, temperature);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    do
    {
        struct MemoryStruct chunk;

        chunk.memory = malloc(1); /* will be grown as needed by the realloc above */
        chunk.size = 0;           /* no data at this point */

        curl = curl_easy_init();
        if (curl)
        {
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth_header);
            headers = curl_slist_append(headers, content_header);
            headers = curl_slist_append(headers, accept_header);

            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, chat_with_llm_helper);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);

            if (res == CURLE_OK)
            {
                json_object *jobj = json_tokener_parse(chunk.memory);

                // Check if the "choices" key exists
                if (json_object_object_get_ex(jobj, "choices", NULL))
                {
                    json_object *choices = json_object_object_get(jobj, "choices");
                    json_object *first_choice = json_object_array_get_idx(choices, 0); //the first element of the array choices
                    const char *data;

                    // The answer begins with a newline character, so we remove it
                    json_object *jobj4 = json_object_object_get(first_choice, "message");
                    json_object *jobj5 = json_object_object_get(jobj4, "content");
                    data = json_object_get_string(jobj5);
                    if (data[0] == '\n')
                        data++;
                    answer = strdup(data); //copy data to answer
                }
                else
                {
                    printf("Error response is: %s\n", chunk.memory);
                    sleep(2); // Sleep for a small amount of time to ensure that the service can recover
                }
                json_object_put(jobj); //free memory
            }
            else
            {
                printf("Error: %s\n", curl_easy_strerror(res));
            }

            curl_slist_free_all(headers); 
            curl_easy_cleanup(curl);
        }

        free(chunk.memory);
    } while ((res != CURLE_OK || answer == NULL) && (--tries > 0));

    if (data != NULL)
    {
        free(data);
    }

    curl_global_cleanup();
    return answer;
}

//construct the prompt for the generate initial seeds, according to the provided RFC document and some examples
// Function: construct_prompt_for_seeds
// Parameters:
//   - protocol_name: Name of the protocol (e.g., "MODBUS", "IEC104")
//   - final_msg: Pointer to a char pointer that will store the final message
//   - seedfile_path: Path to the directory containing seed files
//   - rfc_path: Path to the RFC document file
// Returns:
//   A dynamically allocated string containing the formatted prompt for the LLM,
//   or NULL if an error occurs during file reading or memory allocation.
char *construct_prompt_for_seeds(char *protocol_name, char **final_msg, char *seedfile_path, char *rfc_path)
{   
    char *prompt_grammars = NULL;
    char *msg = NULL;
    char *rfc_content = NULL;

    // Read RFC content
    FILE *fp = fopen(rfc_path, "r");
    if (fp == NULL){
        printf("Error opening file %s\n", rfc_path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *rfc_file_content = malloc(file_size + 1);
    if(rfc_file_content == NULL){
        printf("Error allocating memory for %s\n", rfc_path);
        fclose(fp);
        return NULL;
    }
    fread(rfc_file_content, 1, file_size, fp);
    rfc_file_content[file_size] = '\0';
    fclose(fp);

    // Load example seed files
    char *example_seeds[2] = {NULL, NULL};
    int example_count = 0;
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(seedfile_path)) != NULL) {
        while ((ent = readdir(dir)) != NULL && example_count < 2) {
            if (ent->d_type == DT_REG) {
                char *file_path = malloc(strlen(seedfile_path) + strlen(ent->d_name) + 2);
                sprintf(file_path, "%s/%s", seedfile_path, ent->d_name);
                FILE *seed_file = fopen(file_path, "r");
                if (seed_file != NULL) {
                    fseek(seed_file, 0, SEEK_END);
                    long seed_size = ftell(seed_file);
                    fseek(seed_file, 0, SEEK_SET);
                    example_seeds[example_count] = malloc(seed_size + 1);
                    fread(example_seeds[example_count], 1, seed_size, seed_file);
                    example_seeds[example_count][seed_size] = '\0';
                    fclose(seed_file);
                    example_count++;
                }
                free(file_path);
            }
        }
        closedir(dir);
    }

    // Construct the prompt
    const char *prompt_template =
        "You are a fuzz test expert writing fuzz test cases for the %s protocol.\n"
        "For the %s protocol, refer to the specification document below, output as many different request sequences as possible.\n"
        "Wrap each request example with <sequence></sequence>. There should be one space after every 4 hex characters. Each output must be a valid request sequence. As a special note, the packet length field must match the actual length of the data.\n"
        "For the MODBUS protocol, a request sequence example is (in hex): <sequence>0000 0000 0008 FF16 0004 00F2 0025</sequence>\n"
        "For the IEC104 protocol, a request sequence example is (in hex): <sequence>6804 0700 0000 6804 4300 0000 6804 1300 0000</sequence>\n"
        "%s"
        "The Specification Document of %s protocol is as follows:\n"
        "=== BEGIN SPEC ===\n"
        "%s\n"
        "=== END SPEC ===\n";

    const char *example_template = "Example %d: <sequence>%s</sequence>\n";
    
    char *examples_str = NULL;
    if (example_count > 0) {
        asprintf(&examples_str, "Here are some example seed files for the %s protocol:\n", protocol_name);
        for (int i = 0; i < example_count; i++) {
            char *temp = examples_str;
            asprintf(&examples_str, "%s%s", temp, example_template);
            free(temp);
        }
    } else {
        examples_str = strdup("");
    }

    asprintf(&msg, prompt_template, 
        protocol_name, protocol_name,
        examples_str,
        protocol_name, rfc_file_content);

    free(examples_str);

    *final_msg = msg;

    asprintf(&prompt_grammars, "[{\"role\": \"user\", \"content\": \"%s\"}]", msg);

    free(rfc_file_content);
    if (example_seeds[0]) free(example_seeds[0]);
    if (example_seeds[1]) free(example_seeds[1]);

    return prompt_grammars;
}

//construct the prompt for the grammars of the protocol
char *construct_prompt_for_grammars(char *protocol_name, char **final_msg){
    char *prompt_format = "Desired format: field name <value ranges> | field name <value ranges> | ...";

    char *msg = NULL;
    asprintf(&msg, "For the %s protocol, refer to the specification document, output protocol format and value range of each field, with fields separated by character '|'.\n%s", protocol_name, prompt_format);
    *final_msg = msg;

    char *prompt_grammars = NULL;
    asprintf(&prompt_grammars, "[{\"role\": \"user\", \"content\": \"%s\"}]", msg);

    return prompt_grammars;
}

//extract the grammars from the answer of the LLM
void extract_message_grammars(char *answers, klist_t(gram) * grammar_list)
{

    char *ptr = answers;
    int len = strlen(answers);

    while (ptr < answers + len)
    {
        char *start = strchr(ptr, '[');
        if (start == NULL)
            break;
        char *end = strchr(start, ']');
        if (end == NULL)
            break;
        int count = end - start + 1;
        char *temp = (char *)ck_alloc(count + 1);
        strncpy(temp, start, count);
        temp[count] = '\0';
        ptr = end + 1;

        // conver temp to json object and save it to the list
        json_object *jobj = json_tokener_parse(temp);
        *kl_pushp(gram, grammar_list) = jobj;

        // printf("%s\n", temp);
    }
}

//handing escape characters in the message
char *format_string(char *state_string)
{
    //remove the newline and whiltespace in the beginning of the string if any
    while (state_string[0] == '\n' || state_string[0] == ' ' || state_string[0] == '\t' || state_string[0] == '\r')
    {
        state_string++;
    }

    int len = strlen(state_string);
    while (state_string[len - 1] == '\n' || state_string[len - 1] == '\r' || state_string[len - 1] == ' ' || state_string[len - 1] == '.')
    {
        state_string[len - 1] = '\0';
        len--;
    }

    return state_string;
}


char *construct_prompt_stall(char *protocol_name, char *examples, char *history)
{
    char *template = "In the %s protocol, the communication history between the %s client and the %s server is as follows."
                     "The next proper client request that can affect the server's state are:\\n\\n"
                     "Desired format of real client requests:\\n%sCommunication History:\\n\\\"\\\"\\\"\\n%s\\\"\\\"\\\"";

    char *prompt = NULL;
    asprintf(&prompt, template, protocol_name, protocol_name, protocol_name, examples, history);

    char *final_prompt = NULL;

    asprintf(&final_prompt, "[{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}]", prompt);

    free(prompt);

    return final_prompt;
}

char *construct_prompt_for_remaining_templates(char *protocol_name, char *first_question, char *first_answer)
{
    char *second_question = NULL;
    asprintf(&second_question, "For the %s protocol, other templates of client requests are:", protocol_name);

    json_object *answer_str = json_object_new_string(first_answer);
    // printf("The First Question\n%s\n\n", first_question);
    // printf("The First Answer\n%s\n\n", first_answer);
    // printf("The Second Question\n%s\n\n", second_question);
    const char *answer_str_escaped = json_object_to_json_string(answer_str);

    char *prompt = NULL;

    asprintf(&prompt,
             "["
             "{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"},"
             "{\"role\": \"user\", \"content\": \"%s\"},"
             "{\"role\": \"assistant\", \"content\": %s },"
             "{\"role\": \"user\", \"content\": \"%s\"}"
             "]",
             first_question, answer_str_escaped, second_question);

    json_object_put(answer_str);
    free(second_question);

    return prompt;
}

char *extract_stalled_message(char *message, size_t message_len)
{

    int errornumber;
    size_t erroroffset;
    // After a lot of iterations, the model consistently responds with an empty line and then a line of text
    pcre2_code *extracter = pcre2_compile("\r?\n?.*?\r?\n", PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(extracter, NULL);
    int rc = pcre2_match(extracter, message, message_len, 0, 0, match_data, NULL);
    char *res = NULL;
    if (rc >= 0)
    {
        size_t *ovector = pcre2_get_ovector_pointer(match_data);
        res = strdup(message + ovector[1]);
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(extracter);

    return res;
}


char *format_request_message(char *message)
{
    int message_len = strlen(message);
    int max_len = message_len;
    int res_len = 0;
    char *res = ck_alloc(message_len * sizeof(char));
    for (int i = 0; i < message_len; i++)
    {
        // If an \n is not padded with an \r before, we add it
        if (message[i] == '\n' && (i == 0 || (message[i - 1] != '\r')))
        {
            if (res_len == max_len)
            {
                res = ck_realloc(res, max_len + 10);
                max_len += 10;
            }
            res[res_len++] = '\r';
        }

        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = message[i];
    }

    // Add \r\n\r\n to ensure that the packet is accepted
    for (int i = 0; i < 2; i++)
    {
        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = '\r';
        if (res_len == max_len)
        {
            res = ck_realloc(res, max_len + 10);
            max_len += 10;
        }
        res[res_len++] = '\n';
    }

    if (res_len == max_len)
    {
        res = ck_realloc(res, max_len + 1);
        max_len++;
    }
    res[res_len++] = '\0';
    free(message);
    return res;
}

char *construct_prompt_for_protocol_message_types(char *protocol_name)
{
    /***
     * Prompt to ask the protocol states as follow:
     * ```
     * In the RTSP protocol, the protocol states are:
     *
     * Desired format:
     * <comma_separated_list_of_states_in_uppercase>
     * ```
     * ***/
    char *prompt = NULL;

    // transfer the prompt into string
    asprintf(&prompt, "In the %s protocol, the message types are: \\n\\nDesired format:\\n<comma_separated_list_of_states_in_uppercase_and_without_whitespaces>", protocol_name);

    return prompt;
}

char *construct_prompt_for_requests_to_states(const char *protocol_name,
                                              const char *protocol_state,
                                              const char *example_requests)
{
    /***
     Prompt to ask the sequence of client requests to reach a protocol state as follows:
        ```
        In the RTSP protocol, if the server just starts, to reach the PLAYING state, the sequence of client requests can be:
        DESCRIBE rtsp://127.0.0.1:8554/aacAudioTest RTSP/1.0
        CSeq: 2
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Accept: application/sdp

        SETUP rtsp://127.0.0.1:8554/aacAudioTest/track1 RTSP/1.0
        CSeq: 3
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Transport: RTP/AVP;unicast;client_port=38784-38785

        PLAY rtsp://127.0.0.1:8554/aacAudioTest/ RTSP/1.0
        CSeq: 4
        User-Agent: ./testRTSPClient (LIVE555 Streaming Media v2018.08.28)
        Session: 000022B8
        Range: npt=0.000-

        Similarly, in the RTSP protocol, if the server just starts, to reach the RECORD state, the sequence of client requests can be:
     ***/

    // Transfer formats of example_requests
    json_object *example_requests_json = json_object_new_string(example_requests);
    const char *example_requests_json_str = json_object_to_json_string(example_requests_json);

    json_object *protocol_state_json = json_object_new_string(protocol_state);
    const char *protocol_state_json_str = json_object_to_json_string(protocol_state_json);

    char *prompt = NULL;

    int example_request_len = strlen(example_requests_json_str) - 2;
    if (example_request_len > EXAMPLE_SEQUENCE_PROMPT_LENGTH)
    {
        example_request_len = EXAMPLE_SEQUENCE_PROMPT_LENGTH;
    }

    asprintf(&prompt,
             "In the %s protocol, if the server just starts, to reach the INIT state, the sequence of client requests can be:\\n"
             "%.*s\\nSimilarly, in the %s protocol, if the server just starts, to reach the %.*s state, the sequence of client requests can be:\\n",
             protocol_name,
             example_request_len,
             example_requests_json_str + 1,
             protocol_name,
             (int)strlen(protocol_state_json_str) - 2,
             protocol_state_json_str + 1);

    json_object_put(protocol_state_json);
    json_object_put(example_requests_json);

    return prompt;
}



int parse_pattern(pcre2_code *replacer, pcre2_match_data *match_data, const char *str, size_t len, char *pattern)
{
    strcat(pattern, "(?:");
    // offset == 3;
    int rc = pcre2_match(replacer, str, len, 0, 0, match_data, NULL);

    if (rc < 0)
    {
        switch (rc)
        {
        case PCRE2_ERROR_NOMATCH:
            // printf("No match for %s!\n", str);
            break;
        default:
            // printf("Matching error %d\n", rc);
            break;
        }
        pcre2_match_data_free(match_data);
        pcre2_code_free(replacer);
        return 0;
    }
    // printf("RC is %d\n",rc);
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    // for(int i = 1; i<rc;i++){
    //     printf("Start %d, end %d\n",ovector[2*i],ovector[2*i+1]);
    // }

    if (rc == 4)
    { // matched the first option - there is a special value
        strncat(pattern, str + ovector[2], ovector[3] - ovector[2]);
        // offset += ovector[3] - ovector[2];

        strcat(pattern, "(.*)");
        // offset += 3;

        strncat(pattern, str + ovector[6], ovector[7] - ovector[6]);
        // offset += ovector[7] - ovector[6];
    }
    else if (rc == 5)
    {
        // matched the second option - there is no special value
        strncat(pattern, str + ovector[8], ovector[9] - ovector[8]);
        // offset += ovector[9] - ovector[8];
    }
    else
    {
        FATAL("Regex groups were updated but not the handling code.");
    }
    strcat(pattern, ")");
    return 1;
}

// If successful, puts 2 patterns in the patterns array, the first one is the header, the second is the fields
// Else returns an array with the first element being NULL
char *extract_message_pattern(const char *header_str, khash_t(field_table) * field_table, pcre2_code **patterns, int debug_file, const char *debug_file_name)
{
    int errornumber;
    size_t erroroffset;
    char header_pattern[128] = {0};
    char fields_pattern[1024] = {0};
    pcre2_code *replacer = pcre2_compile("(?:(.*)(?:<<(.*)>>)(.*))|(.+)", PCRE2_ZERO_TERMINATED, PCRE2_DOTALL, &errornumber, &erroroffset, NULL);
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(replacer, NULL);
    char *message_type = NULL;
    // int offset = 0;
    /**
     * Example output
     * patterns[0] = (?:PLAY (.*)\r\n)
     * patterns[1] = (?|(?:CSeq: (.*)\r\n)|(?:User-Agent: (.*)\r\n)|(?:Range: (.*)\r\n)|(?:\r\n))
     */

    {
        // We use the string in such an escaped format for easier debugging as the regex library supports parsing it properly
        // The string contains quotations so they are ignored
        header_str++;

        int message_len = 0;
        while (header_str[message_len] != '\0' 
        && header_str[message_len] != ' ' 
        && header_str[message_len] != '\n' 
        && header_str[message_len] != '\r' 
        && header_str[message_len] != '\\' )
        {
            message_len++;
        }
        message_type = ck_alloc(message_len + 1);
        memcpy(message_type, header_str, message_len);
        message_type[message_len] = '\0';

        size_t len = strlen(header_str) - 1;
        strcat(header_pattern, "^"); // Ensure that it captures the start of the string
        if (!parse_pattern(replacer, match_data, header_str, len, header_pattern))
        {
            patterns[0] = NULL;
            return NULL;
        }
    }

    int first = 1;

    strcat(fields_pattern, "(?|");
    for (khiter_t field_t_iter = kh_begin(field_table); field_t_iter != kh_end(field_table); ++field_t_iter)
    {
        if (!kh_exist(field_table, field_t_iter) || kh_value(field_table, field_t_iter) < (TEMPLATE_CONSISTENCY_COUNT / 2 + (TEMPLATE_CONSISTENCY_COUNT % 2)))
            continue;

        if (!first)
        {
            strcat(fields_pattern, "|");
        }
        else
        {
            first = 0;
        }

        json_object *field_v = json_object_new_string(kh_key(field_table, field_t_iter));
        const char *str = json_object_to_json_string(field_v);
        // We use the string in such an escaped format for easier debugging as the regex library supports parsing it properly
        // The string contains quotations so they are ignored
        str++;
        size_t len = strlen(str) - 1;
        int matched = parse_pattern(replacer, match_data, str, len, fields_pattern);
        json_object_put(field_v);
        if (!matched)
        {
            patterns[0] = NULL;
            return NULL;
        }
    }

    strcat(fields_pattern, ")");

    if (first == 1)
    { // convert from (?|) to (.+) when the group is empty
        fields_pattern[1] = '.';
        fields_pattern[2] = '+';
    }

    pcre2_match_data_free(match_data);
    pcre2_code_free(replacer);
    printf("Header pattern is %s\n", header_pattern);
    printf("Fields pattern is %s\n", fields_pattern);

    if (debug_file != -1 && debug_file_name != NULL)
    {
        ck_write(debug_file, header_pattern, strlen(header_pattern), debug_file_name);
        ck_write(debug_file, "\n", 1, debug_file_name);
        ck_write(debug_file, fields_pattern, strlen(fields_pattern), debug_file_name);
    }

    {
        pcre2_code *p = pcre2_compile(header_pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
        pcre2_jit_compile(p, PCRE2_JIT_COMPLETE);
        patterns[0] = p;
    }
    {
        pcre2_code *p = pcre2_compile(fields_pattern, PCRE2_ZERO_TERMINATED, 0, &errornumber, &erroroffset, NULL);
        pcre2_jit_compile(p, PCRE2_JIT_COMPLETE);
        patterns[1] = p;
    }
    return message_type;
}

range_list starts_with(char *line, int length, pcre2_code *pattern)
{
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(pattern, NULL);

    int rc = pcre2_match(pattern, line, length, 0, 0, match_data, NULL); // find the first range

    // printf("starts_with rc is %d\n", rc);
    if (rc < 0)
    {
        switch (rc)
        {
        case PCRE2_ERROR_NOMATCH:
            // printf("No match!\n");
            break;
        default:
            // printf("Matching error %d\n", rc);
            break;
        }
        pcre2_match_data_free(match_data);
        range_list res;
        kv_init(res);
        return res;
    }

    range_list dyn_ranges;
    kv_init(dyn_ranges);
    PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
    for (int i = 1; i < rc; i++)
    {
        if (ovector[2 * i] == -1)
            continue;
        // printf("Group %d %d %d\n",i, ovector[2 * i], ovector[2 * i + 1]);
        range v = {.start = ovector[2 * i], .len = ovector[2 * i + 1] - ovector[2 * i], .mutable = 1};
        kv_push(range, dyn_ranges, v);
        // kv_push(range, dyn_ranges, v);
        //  ranges[0][i - 1] = v;
    }
    range v = {.start = ovector[0], .len = ovector[1] - ovector[0], .mutable = 1};
    kv_push(range, dyn_ranges, v); // add the global range at the end

    pcre2_match_data_free(match_data);
    return dyn_ranges;
}

range_list get_mutable_ranges(char *line, int length, int offset, pcre2_code *pattern)
{
    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(pattern, NULL);

    range_list dyn_ranges;
    kv_init(dyn_ranges);

    for (;;) // catch all the other ranges
    {
        int rc = pcre2_match(pattern, line, length, offset, 0, match_data, NULL);
        if (rc < 0)
        {
            switch (rc)
            {
            case PCRE2_ERROR_NOMATCH:
                // printf("No match!\n");
                break;
            default:
                // printf("Matching error %d\n", rc);
                break;
            }
            pcre2_match_data_free(match_data);
            match_data = NULL;
            break;
        }
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        if (offset != ovector[0])
        {
            range v = {.start = offset, .len = ovector[0] - offset, .mutable = 1};
            kv_push(range, dyn_ranges, v);
        }

        // printf("Matched over %d %d\n", ovector[0], ovector[1]);
        for (int i = 1; i < rc; i++)
        {
            if (ovector[2 * i] == -1)
                continue;
            // printf("Group %d %d %d\n",i, ovector[2 * i], ovector[2 * i + 1]);
            range v = {.start = ovector[2 * i], .len = ovector[2 * i + 1] - ovector[2 * i], .mutable = 1};
            kv_push(range, dyn_ranges, v);
            // ranges[0][i - 1] = v;
        }
        if (offset == ovector[1])
        { // in the case the match is empty, we just move a step forward
            offset++;
        }
        else
        {
            offset = ovector[1];
        }
    }

    if (offset < length) // catch anything past the last matched pattern
    {
        range v = {.start = offset, .len = length - offset, .mutable = 1};
        kv_push(range, dyn_ranges, v);
    }

    if (match_data != NULL)
    {
        pcre2_match_data_free(match_data);
    }
    return dyn_ranges;
}

char *unescape_string(const char *input)
{
    size_t length = strlen(input);
    char *output = (char *)malloc((length + 1) * sizeof(char));

    if (output == NULL)
    {
        printf("Memory allocation failed.\n");
        return NULL;
    }

    size_t i, j = 0;
    for (i = 0; i < length; i++)
    {
        if (input[i] == '\\')
        {
            i++; // Skip the backslash
            switch (input[i])
            {
            case 'n':
                output[j++] = '\n';
                break;
            case 't':
                output[j++] = '\t';
                break;
            case 'r':
                output[j++] = '\r';
                break;
            case '\\':
                output[j++] = '\\';
                break;
            default:
                output[j++] = input[i];
                break;
            }
        }
        else
        {
            output[j++] = input[i];
        }
    }

    output[j] = '\0'; // Add null-terminator to the output string
    return output;
}

// Add this function to extract and save sequences
void extract_and_save_sequences(const char *llm_output, const char *output_dir) {
    const char *start_tag = "<sequence>";
    const char *end_tag = "</sequence>";
    const char *ptr = llm_output;
    int sequence_count = 0;

    // Create the output directory if it doesn't exist
    mkdir(output_dir, 0777);

    while ((ptr = strstr(ptr, start_tag)) != NULL) {
        ptr += strlen(start_tag);
        const char *end = strstr(ptr, end_tag);
        
        if (end == NULL) {
            break;
        }

        size_t len = end - ptr;
        char *sequence = malloc(len + 1);
        strncpy(sequence, ptr, len);
        sequence[len] = '\0';

        // Remove any whitespace from the sequence
        char *cleaned_sequence = malloc(len + 1);
        int j = 0;
        for (int i = 0; i < len; i++) {
            if (!isspace(sequence[i])) {
                cleaned_sequence[j++] = sequence[i];
            }
        }
        cleaned_sequence[j] = '\0';

        // Create the output file
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/sequence_%d.bin", output_dir, ++sequence_count);
        FILE *fp = fopen(filename, "wb");
        
        if (fp == NULL) {
            printf("Error creating file %s\n", filename);
            free(sequence);
            free(cleaned_sequence);
            continue;
        }

        // Convert hex string to binary and write to file
        for (int i = 0; i < strlen(cleaned_sequence); i += 2) {
            char hex[3] = {cleaned_sequence[i], cleaned_sequence[i+1], '\0'};
            int value = strtol(hex, NULL, 16);
            fputc(value, fp);
        }

        fclose(fp);
        free(sequence);
        free(cleaned_sequence);

        ptr = end + strlen(end_tag);
    }
}

// Modify the write_new_seeds function to use extract_and_save_sequences
void write_new_seeds(char *enriched_file, char *contents) {
    // Create a directory for the extracted sequences
    char *sequences_dir = NULL;
    asprintf(&sequences_dir, "%s_sequences", enriched_file);

    // Extract and save sequences
    extract_and_save_sequences(contents, sequences_dir);

    free(sequences_dir);

    // Keep the original file writing logic
    FILE *fp = fopen(enriched_file, "w");
    if (fp == NULL) {
        printf("Error in opening the file %s\n", enriched_file);
        exit(1);
    }

    // remove the newline and whiltespace in the beginning of the string if any
    while (contents[0] == '\n' || contents[0] == ' ' || contents[0] == '\t' || contents[0] == '\r')
    {
        contents++;
    }

    // Check if last 4 characters of the client_request_answer string are \r\n\r\n
    // If not, add them
    int len = strlen(contents);
    if (contents[len - 1] != '\n' || contents[len - 2] != '\r' || contents[len - 3] != '\n' || contents[len - 4] != '\r')
    {
        fprintf(fp, "%s\r\n\r\n", contents);
    }
    else
    {
        fprintf(fp, "%s", contents);
    }

    fclose(fp);
}


/***
 * Get the protocol states based on self-consistency check
 * pass the parameters: protocol_name, states_set, states_string
 ***/
void get_protocol_message_types(char *state_prompt, khash_t(strSet) * states_set)
{
    khash_t(strMap) *state_to_times = kh_init(strMap); // map from state to times

    for (int i = 0; i < CONFIDENT_TIMES; i++)
    {
        char *state_answer = chat_with_llm(state_prompt, "instruct", MESSAGE_TYPE_RETRIES, 0.5);
        if (state_answer == NULL)
            continue;
        // printf("## Answer from LLM:\n %s\n", state_answer);

        state_answer = format_string(state_answer);

        char *state_tokens = strtok(state_answer, ",");
        while (state_tokens != NULL)
        {
            char *protocol_state = state_tokens;
            protocol_state = format_string(protocol_state);
            // save the state to the map
            int ret;
            khiter_t k = kh_put(strMap, state_to_times, protocol_state, &ret);
            if (ret == 0)
            {
                kh_value(state_to_times, k)++;
            }
            else
            {
                kh_value(state_to_times, k) = 1;
            }

            state_tokens = strtok(NULL, ",");
        }
    }

    // traverse the map and get the states whose times are larger than 0.5 * CONFIDENT_TIMES
    for (khiter_t k = kh_begin(state_to_times); k != kh_end(state_to_times); ++k)
    {
        if (kh_exist(state_to_times, k))
        {
            if (kh_value(state_to_times, k) >= 0.5 * CONFIDENT_TIMES)
            {
                const char *protocol_state = kh_key(state_to_times, k);
                // add the state to the set
                int ret;
                kh_put(strSet, states_set, protocol_state, &ret);
            }
        }
    }
}

khash_t(strSet) * duplicate_hash(khash_t(strSet) * set)
{
    khash_t(strSet) *new_set = kh_init(strSet);

    for (khiter_t k = kh_begin(set); k != kh_end(set); ++k)
    {
        if (kh_exist(set, k))
        {
            const char *val = kh_key(set, k);
            int ret;
            kh_put(strSet, new_set, val, &ret);
        }
    }

    return new_set;
}

// message_set_list generate_combinations(khash_t(strSet)* sequence, int size)
// {
//     if(size == 0)
//     {
//         message_set_list output;
//         kv_init(output);
//         kv_push(khash_t(strSet)*,output,kh_init(strSet));
//         return output;
//     }
//     else
//     {
//         message_set_list subcombinations = generate_combinations(sequence,size-1);
//         message_set_list newCombinations;
//         kv_init(newCombinations);
//         for(int i = 0; i < kv_size(subcombinations);i++)
//         {
//             khash_t(strSet)* target = kv_A(subcombinations,i);
//             khiter_t sequence_iter;
//             for (sequence_iter = kh_begin(sequence); sequence_iter != kh_end(sequence); sequence_iter++)
//             {
//                 if (!kh_exist(sequence, sequence_iter))
//                     continue;
//                 khiter_t k = kh_get(strSet, target, kh_val(sequence,sequence_iter));
//                 if (kh_exist(target, k))
//                     continue;
//                 khash_t(strSet)* newCombination = duplicate_hash(target);
//                 int absent;
//                 kh_put(strSet,newCombination,kh_val(sequence,sequence_iter))    
//             }
//         }
//         return newCombinations;
//     }
// }
void make_combination(khash_t(strSet)* sequence, char** data , message_set_list* res,khiter_t st, khiter_t end, int index, int size);

message_set_list message_combinations(khash_t(strSet)* sequence, int size)
{
    message_set_list res;
    kv_init(res);
    char* data[size];
    make_combination(sequence,data, &res, kh_begin(sequence), kh_end(sequence), 0, size);
    return res;
}

void make_combination(khash_t(strSet)* sequence, char** data , message_set_list* res,khiter_t st, khiter_t end,
                     int index, int size)
{

    if (index == size)
    {
        khash_t(strSet)* combination = kh_init(strSet);
        int absent;
        for (int j=0; j<size; j++){
            kh_put(strSet,combination, data[j],&absent );
        }
        kv_push(khash_t(strSet)*,*res,combination);
        return;
    }
    for (khiter_t i=st; i != end && end-i+1 >= size-index; i++)
    {
        if(!kh_exist(sequence,i))
            continue;
        data[index] = kh_key(sequence,i);
        make_combination(sequence, data,res, i+1, end, index+1, size);
    }
}



int min(int a, int b) {
    return a < b ? a : b;
}

char *enrich_sequence(char *sequence, khash_t(strSet) * missing_message_types)
{
    const char *prompt_template =
        "The following is one sequence of client requests:\\n"
        "%.*s\\n"
        "Please add the %.*s client requests in the proper locations, and the modified sequence of client requests is:";

    int missing_fields_len = 0;
    int missing_fields_capacity = 100;
    char *missing_fields_seq = ck_alloc(missing_fields_capacity);

    khiter_t k;
    int i = 0;
    for (k = kh_begin(missing_message_types); 
    k != kh_end(missing_message_types) && i < min(MAX_ENRICHMENT_MESSAGE_TYPES, kh_size(missing_message_types)); 
    ++k)
    {
        if (!kh_exist(missing_message_types, k))
            continue;
        ++i; // Increment only after seeing a message type
        const char *message_type = kh_key(missing_message_types, k);
        int needed_len = strlen(message_type) + 2; // add for the ', '

        if (missing_fields_len + needed_len > missing_fields_capacity)
        {
            missing_fields_capacity += 2 * needed_len;
            missing_fields_seq = ck_realloc(missing_fields_seq, missing_fields_capacity);
        }

        memcpy(missing_fields_seq + missing_fields_len, message_type, strlen(message_type));
        memcpy(missing_fields_seq + missing_fields_len + needed_len - 2, ", ", 2);

        missing_fields_len += needed_len;
    }
    missing_fields_len -= 2; // ignore the last ', '

    char *prompt = NULL;

    json_object *sequence_escaped = json_object_new_string(sequence);
    const char *sequence_escaped_str = json_object_to_json_string(sequence_escaped);
    sequence_escaped_str++;

    int sequence_len = strlen(sequence_escaped_str) - 1;
    int allowed_tokens = (MAX_TOKENS - strlen(prompt_template) - missing_fields_len);
    if (sequence_len > allowed_tokens)
    {
        sequence_len = allowed_tokens;
    }
    asprintf(&prompt, prompt_template, sequence_len, sequence_escaped_str, missing_fields_len, missing_fields_seq);
    ck_free(missing_fields_seq);
    json_object_put(sequence_escaped);

    char *response = chat_with_llm(prompt, "instruct", ENRICHMENT_RETRIES, 0.5);

    free(prompt);

    return response;
}

// // For debugging
// // gcc -g -o chat-llm chat-llm.c chat-llm.h -lcurl -ljson-c -lpcre2-8
// int main(int argc, char **argv)
// {
//     char *protocol_name = argv[1];
//     char *in_dir = argv[2];
//     khash_t(strSet) *states_set = kh_init(strSet);

//     char *state_prompt = construct_prompt_for_protocol_states(protocol_name);

//     // Get protocol states
//     get_protocol_message_types(state_prompt, states_set);

//     // traverse the states_set
//     khiter_t k;
//     for (k = kh_begin(states_set); k != kh_end(states_set); ++k)
//     {
//         if (kh_exist(states_set, k))
//         {
//             const char *protocol_state = kh_key(states_set, k);
//             printf("## State_traverse: %s\n", protocol_state);
//         }
//     }

//     // Get seeds to states and save them to the in_dir
//     get_seeds_to_states(in_dir, states_set, protocol_name);

//     // char *prompt = NULL;
//     // asprintf(&prompt, "user: The colors of flowers:\\nassistant: red and yellow.\\nuser: Other colors are:");
//     // printf("## Prompt to LLM:\n %s\n", prompt);
//     // char *answer = chat_with_llm(prompt, "instruct");
//     // printf("## Answer from LLM:\n %s\n", answer);

//     char *protocol_name = argv[1];
//     khash_t(consistency_table) *const_table = kh_init(consistency_table);
//     klist_t(rang) *protocol_patterns = kl_init(rang);

//     for (int iter = 0; iter < 5; iter++)
//     {

//         char *templates_prompt = construct_prompt_for_templates(protocol_name);
//         char *templates_answer = chat_with_llm(templates_prompt, "turbo");
//         // printf("## Answer from LLM:\n %s\n", templates_answer);
//         char *remaining_prompt = construct_prompt_for_remaining_templates(protocol_name, templates_prompt, templates_answer);
//         // printf("remaining prompt is:\n %s\n", remaining_prompt);
//         char *remaining_templates = chat_with_llm(remaining_prompt, "turbo");
//         // printf("## Remaining templates:\n %s\n", remaining_templates);

//         char *combined_templates = NULL;
//         asprintf(&combined_templates, "%s\n%s", templates_answer, remaining_templates);

//         printf("The final info is\n%s\n", combined_templates);
//         klist_t(gram) *grammar_list = kl_init(gram);
//         extract_message_grammars(combined_templates, grammar_list);

//         kliter_t(gram) * iter;
//         for (iter = kl_begin(grammar_list); iter != kl_end(grammar_list); iter = kl_next(iter))
//         {
//             json_object *jobj = kl_val(iter);

//             json_object *header = json_object_array_get_idx(jobj, 0);

//             int absent;

//             const char *header_str = json_object_get_string(header);

//             khiter_t k = kh_put(consistency_table, const_table, header_str, &absent);
//             if (absent)
//             {
//                 khash_t(field_table) *field_table = kh_init(field_table);
//                 kh_value(const_table, k) = field_table;
//             }

//             for (int i = 1; i < json_object_array_length(jobj); i++)
//             {
//                 const char *v = json_object_get_string(json_object_array_get_idx(jobj, i));
//                 khash_t(field_table) *field_table = kh_value(const_table, k);
//                 khiter_t field_k = kh_put(field_table, field_table, v, &absent);
//                 if (absent)
//                 {
//                     kh_value(field_table, field_k) = 0;
//                 }
//                 kh_value(field_table, field_k)++;
//             }
//         }
//         kl_destroy_gram(grammar_list);
//     }

//     for (khiter_t con_t_iter = kh_begin(const_table); con_t_iter != kh_end(const_table); ++con_t_iter)
//     {
//         if (kh_exist(const_table, con_t_iter))
//         {
//             pcre2_code **patterns = ck_alloc(2 * sizeof(pcre2_code *));

//             khash_t(field_table) *field_table = kh_value(const_table, con_t_iter);
//             const char* header_str = json_object_to_json_string(json_object_new_string(kh_key(const_table, con_t_iter)));

//             extract_message_pattern_k(header_str,field_table, patterns);
//             *kl_pushp(rang, protocol_patterns) = patterns;
//         }
//     }

//     char *demo_lines[] = {

//         "DESCRIBE 123\r\n"
//         "CSeq: 1212313\r\n"
//         "User-Agent: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n",

//         "DESCRIBE 123\r\n"
//         "DESCRIBE 123\r\n"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n",

//         "DESCRIBE 123\r\n"
//         "1231321321321"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r\n"
//         "Accept: 1212313\r\n"
//         "\r\n"
//         "1231321321321",

//         "DESCRIBE 123\r\n"
//         "1231321321321"
//         "User-Agent: 1212313\r\n"
//         "CSeq: 1212313\r1231321321321\n"
//         "Accept: 1212313\r\n"
//         "\r\n"
//         "1231321321321",

//         "PLAY 123\r\n"
//         "CSeq: 1212313\r\n"
//         "DESCRIBE 123\r\n"
//         "User-Agent: 1212313\r\n"
//         "Session: 1212313\r\n"
//         "Range: 1212313\r\n"
//         "\r\n",

//     };

// char* answers = "For the RTSP protocol, the DESCRIBE client request template is:"
//     "{\"DESCRIBE\":\"string\\r\\n\",\"CSeq:\":\"integer\\r\\n\",\"User-Agent:\":\"string\\r\\n\",\"Accept:\":\"string\\r\\n\\r\\n\"}."
//     "For the RTSP protocol, the DESCRIBE client request template is:{\"DESCRIBE\":\"string\\r\\n\",\"CSeq:\":\"integer\\r\\n\",\"User-Agent:\":\"string\\r\\n\",\"Accept:\":\"string\\r\\n\\r\\n\"}";

// for (int demo = 0; demo < sizeof(demo_lines) / sizeof(char *); demo++)
// {
//     printf("\nTrying to match \n%s\n\n", demo_lines[demo]);
//     int max_rc = -1;
//     kliter_t(rang) * iter_rang;
//     range_list max_ranges;
//     int i = 0;
//     for (iter_rang = kl_begin(protocol_patterns); iter_rang != kl_end(protocol_patterns); iter_rang = kl_next(iter_rang),i++)
//     {
//         // printf("Compare! \n");

//         pcre2_code **patterns = kl_val(iter_rang);
//         pcre2_code *header_pattern = patterns[0];
//         pcre2_code *fields_pattern = patterns[1];

//         range_list header_ranges = starts_with(demo_lines[demo], strlen(demo_lines[demo]), header_pattern);
//         kv_init(header_ranges);

//         if (kv_size(header_ranges) == 0)
//         {
//             printf("Demo %d Did not match pattern %d\n", demo, i);
//             continue;
//         }
//         else
//         {
//             printf("Demo %d Did matched pattern %d\n", demo, i);
//             range header_match = kv_pop(header_ranges);
//             char *offsetted_line = demo_lines[demo];
//             size_t offsetted_len = strlen(demo_lines[demo]);
//             range_list field_ranges = get_mutable_ranges(offsetted_line,offsetted_len, header_match.len,fields_pattern);

//             for(int i = 0; i < kv_size(field_ranges);i++){
//                 kv_push(range, header_ranges, kv_A(field_ranges,i));
//             }
//             kv_destroy(field_ranges);

//             max_ranges = header_ranges;

//             break;
//         }
//     }

//     if (max_rc != -1)
//     {
//         printf("Matched! \n");
//         for (int i = 0; i < max_rc; i++)
//         {
//             printf("start=%d len=%d mutable=%d\n", kv_A(max_ranges,i).start,kv_A(max_ranges,i).len, kv_A(max_ranges,i).mutable);
//             printf("content=%s\n", json_object_to_json_string(json_object_new_string_len(demo_lines[demo] + kv_A(max_ranges,i).start, kv_A(max_ranges,i).len)));
//         }
//     }
//     else
//     {
//         printf("No matches\n");
//     }
// }

// Traverse the list

//     return 0;
// }
