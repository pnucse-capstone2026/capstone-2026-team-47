#include "nvmf_tokenize.h"
#include "spdk/nvmf_cmd.h"

static const char *model_path = NULL;
static const char *postgres_uri = NULL;
const struct option NVMF_TOKENIZE_LONG_OPTIONS[] = {
    {"model-path", required_argument, NULL, 1557},
    {"postgres-uri", required_argument, NULL, 1558},
    {NULL},
};

static void nvmf_tokenizer_usage(void) {
  fprintf(stderr, "    --model-path <path-to-gguf-model> \n");
  fprintf(stderr, "    --postgres-uri <postgresql-uri> \n");
}

static int nvmf_parse_arg(int ch, char *arg) {
  if(ch == 1557) {
    model_path = arg;
  } else {
    postgres_uri = arg;
  }
  return 0;
}

/* called after subsystem init */
static void nvmf_tokenizer_started(void *arg1) {
  if (getenv("MEMZONE_DUMP") != NULL) {
    spdk_memzone_dump(stdout);
    fflush(stdout);
  }

  spdk_nvmf_set_custom_io_cmd_hdlr(0xD5, &nvmf_bdev_ctrlr_tokenize_cmd,
                                   SPDK_NVME_DATA_HOST_TO_CONTROLLER);
}

/* custom cleanup callback */
static void nvmf_tokenizer_shutdown(void) {}


struct tokenizer_ctx *g_tokenizer_ctx = NULL;

int main(int argc, char **argv) {
  int rc = 0;
  struct spdk_app_opts opts = {
    .shutdown_cb = nvmf_tokenizer_shutdown,
  };

  spdk_app_opts_init(&opts, sizeof(opts));
  
  opts.name = "nvmf_tokenizer";
  if ((rc = spdk_app_parse_args(
           argc, argv, &opts, NULL, NVMF_TOKENIZE_LONG_OPTIONS, nvmf_parse_arg,
           nvmf_tokenizer_usage)) != SPDK_APP_PARSE_ARGS_SUCCESS) {
    exit(rc);
  }

  if (!model_path) {
    fprintf(stderr, "must specify '--model-path' option.");
    exit(1);
  }
  
  if (!postgres_uri) {
    fprintf(stderr, "must specify '--postgres-uri' option.");
    exit(1);
  }

  g_tokenizer_ctx = tokenizer_init(model_path, postgres_uri);


  /* Blocks until the application is exiting */
  rc = spdk_app_start(&opts, nvmf_tokenizer_started, NULL);
  spdk_app_fini();

  /* app cleanup routine */
  tokenizer_free(g_tokenizer_ctx);
  return rc;
}

