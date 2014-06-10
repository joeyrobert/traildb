
#define _GNU_SOURCE

#include <sys/stat.h>
#include <sys/types.h>

#include "breadcrumbs_encoder.h"

#define MAX_NUM_FIELDS 255
#define MAX_NUM_INPUTS 10000000
#define TSTAMP_MAX (UINT32_MAX - 1)
#define ARENA_INCREMENT 10000000
#define INVALID_RATIO 0.01

struct arena{
    void *data;
    uint32_t size;
    uint32_t next;
    uint32_t item_size;
};

static Pvoid_t cookie_index;
static Pvoid_t *lexicon;
static Word_t *lexicon_counters;

static struct arena cookies = {.item_size = sizeof(struct cookie)};
static struct arena loglines = {.item_size = sizeof(struct logline)};
static struct arena fields = {.item_size = 4};

static inline void *get_item(struct arena *a, uint32_t idx)
{
    return a->data + a->item_size * idx;
}

static void *add_item(struct arena *a)
{
    if (a->next >= a->size){
        a->size += ARENA_INCREMENT;
        if (!(a->data = realloc(a->data, a->item_size * a->size)))
            DIE("Realloc failed\n");
    }
    return get_item(a, a->next++);
}

static long parse_int_arg(const char *str, const char *type, long max)
{
    char *endp;
    long n = strtol(str, &endp, 10);
    if (n < 1)
        DIE("invalid number of %s: %s\n", type, str);
    if (n >= max)
        DIE("too many %s: %ld >= %ld\n", type, n, max);
    return n;
}

static int parse_line(char *line, long num_fields)
{
    int i;
    char *tmp;
    int64_t tstamp;
    Word_t *cookie_ptr;
    struct cookie *cookie;
    struct logline *logline;
    char *cookie_str = strsep(&line, " ");

    if (strlen(cookie_str) != 32 || !line)
        return 1;

    tstamp = strtoll(strsep(&line, " "), &tmp, 10);

    if (tstamp < 0 || tstamp > TSTAMP_MAX)
        return 1;

    JSLI(cookie_ptr, cookie_index, (uint8_t*)cookie_str);
    if (!*cookie_ptr){
        cookie = (struct cookie*)add_item(&cookies);
        memset(cookie, 0, cookies.item_size);
        *cookie_ptr = cookies.next;
    }
    cookie = (struct cookie*)get_item(&cookies, *cookie_ptr - 1);
    logline = (struct logline*)add_item(&loglines);
    cookie->last = logline;

    logline->fields_offset = fields.next;
    logline->num_fields = 0;
    logline->timestamp = (uint32_t)tstamp;
    logline->prev = cookie->last;

    for (i = 0; line && i < num_fields - 2; i++){
        char *field = strsep(&line, " ");
        uint32_t field_size = strlen(field);
        uint32_t value = i + 1;

        if (field_size >= MAX_FIELD_SIZE)
            field[MAX_FIELD_SIZE - 1] = 0;

        if (field_size > 0){
            Word_t *token_id;
            JSLI(token_id, lexicon[i], (uint8_t*)field);
            if (!*token_id){
                *token_id = ++lexicon_counters[i];
                if (*token_id >= (1 << 24) - 1)
                    DIE("Too many values for the %dth field!\n", i);
            }
            value |= (*token_id) << 8;
        }

        if (cookie->previous_values[i] != value){
            *((uint32_t*)add_item(&fields)) = value;
            cookie->previous_values[i] = value;
            ++logline->num_fields;
        }
    }

    /* lines with too few or too many fields are considered invalid */
    if (i < num_fields - 2 || line != NULL)
        return 1;
    else
        return 0;
}

static void read_inputs(long num_fields, long num_inputs)
{
    char *line = NULL;
    size_t line_size = 0;
    uint32_t num_lines = 0;
    uint32_t num_invalid = 0;

    while (num_inputs){
        ssize_t n = getline(&line, &line_size, stdin);

        if (n < 1)
            DIE("Truncated input\n");

        /* remove trailing newline */
        line[n - 1] = 0;

        if (!strcmp(line, "**DONE**"))
            --num_inputs;
        else{
            ++num_lines;
            if (parse_line(line, num_fields))
                ++num_invalid;
        }
    }

    if (num_invalid / (float)num_lines > INVALID_RATIO)
        DIE("Too many invalid lines: %u invalid, %u valid\n",
            num_invalid,
            num_lines);
    else
        fprintf(stderr,
                "All inputs consumed successfully: "
                "%u valid lines, %u invalid\n",
                num_lines,
                num_invalid);
}

static void encode_lexicons(int num_lexicons, const char *root)
{
    char *path;
    int i;
    for (i = 0; i < num_lexicons; i++){
        asprintf(&path, "%s/lexicon.%d", root, i);
        encode_lexicon(lexicon[i], path);
        free(path);
    }
    asprintf(&path, "%s/cookies", root);
    encode_lexicon(cookie_index, path);
    free(path);
}

int main(int argc, char **argv)
{
    long num_fields;
    long num_inputs;

    if (argc < 4)
        DIE("Usage: breadcrumbs_encoder "
            "number-of-fields number-of-inputs output-dir\n");

    num_fields = parse_int_arg(argv[1], "fields", MAX_NUM_FIELDS);
    num_inputs = parse_int_arg(argv[2], "inputs", MAX_NUM_INPUTS);

    /* Open our own stdin for writing: This ensures that FIFO won't
       get EOF'ed ever. */
    FILE *in = fopen("/dev/stdin", "r+");
    if (!in)
        DIE("Could not initialize stdin\n");

    /* Opportunistically try to create the output directory. We don't
       care if it fails, e.g. because it already exists */
    mkdir(argv[3], 0755);

    cookies.item_size += (num_fields - 2) * 4;

    if (!(lexicon = calloc(num_fields - 2, sizeof(Pvoid_t))))
        DIE("Could not allocate lexicon\n");

    if (!(lexicon_counters = calloc(num_fields - 2, sizeof(Word_t))))
        DIE("Could not allocate lexicon counters\n");

    read_inputs(num_fields, num_inputs);
    encode_lexicons(num_fields - 2, argv[3]);

    return 0;
}

