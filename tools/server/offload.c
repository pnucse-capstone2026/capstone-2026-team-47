#define _GNU_SOURCE
#include <stdio.h>
#include <inttypes.h>

/* for file open/close */
#include <unistd.h>
#include <fcntl.h>

/* for nvme passthrough */
#include <libnvme.h>
#include <stdlib.h>

#include <assert.h>

/* for fiemap */
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include "offload.h"

/* libnvme latest stable(version 1.16.1) 기준으로 작성 */
/* HEAD는 api가 상당히 많이 바뀌었음 */

#define FIEMAP_EXTENT_COUNT 4096

#define OUTPUT_BUF_SIZE (128 * 1024)

struct extent {
  uint64_t start;
  uint64_t length;
};

struct extent_info {
  uint64_t nr_extents; 		/* number of extents */
  uint64_t total_nr_blocks;
  struct extent exts[FIEMAP_EXTENT_COUNT];
};

static struct extent_info *get_extent_info(int fd) {
  struct fiemap *fm =
      MUST_NONNULL(malloc(sizeof(struct fiemap) +
                          sizeof(struct fiemap_extent[FIEMAP_EXTENT_COUNT])),
                   "fiemap alloc failed");

  struct extent_info *ret = MUST_NONNULL(
      malloc(sizeof(struct extent_info)), "extent_info alloc failed");
  
  /* 각 flag 관련 설명: https://www.kernel.org/doc/Documentation/filesystems/fiemap.txt */
  const uint32_t allowed_flags = FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_UNWRITTEN;
  
  ASSERT(fd >= 0, "invalid fd");

  *fm = (struct fiemap){
    .fm_start = 0,
    .fm_length = ~0ULL, /* get mapping as many as possible */
    .fm_extent_count = FIEMAP_EXTENT_COUNT,
    .fm_flags = FIEMAP_FLAG_SYNC, /* fsync before fiemap */
  };

  MUST_SUCCESS_POSIX(ioctl(fd, FS_IOC_FIEMAP, fm));
  ASSERT(fm->fm_mapped_extents, "no extent found");


  *ret = (struct extent_info){
      .nr_extents = fm->fm_mapped_extents,
  };

  ASSERT(fm->fm_extents[fm->fm_mapped_extents - 1].fe_flags & FIEMAP_EXTENT_LAST,
         "failed to fetch every extents. 'FIEMAP_EXTENT_COUNT' too small.");

  
  for (uint64_t i = 0; i < fm->fm_mapped_extents; ++i) {
    const struct fiemap_extent* src = &fm->fm_extents[i];
    struct extent *dest = &ret->exts[i];
    *dest = (struct extent){
        .start = src->fe_physical / 512,
        .length = src->fe_length / 512,
    };

    ret->total_nr_blocks += dest->length;
    
    ASSERT(!(src->fe_flags & ~(allowed_flags)), "unexpected extent matched");
  }

  free(fm);
  return ret;
}

static void init_nvme_cmd(const struct extent_info* input, const struct extent_info* output,
			  struct nvme_passthru_cmd64* cmd) {
  size_t input_bytes = offsetof(struct extent_info, exts) +
                       sizeof(struct extent[input->nr_extents]);

  size_t output_bytes = offsetof(struct extent_info, exts) +
                       sizeof(struct extent[output->nr_extents]);

  size_t send_buf_size = input_bytes + output_bytes;

  char* send_buf = MUST_NONNULL(malloc(input_bytes + output_bytes), "allocation failure");

  memcpy(send_buf, input, input_bytes);
  memcpy(send_buf + input_bytes, output, output_bytes);

  *cmd = (struct nvme_passthru_cmd64) {
    .opcode = 0xD5, /* bpe command opcode */
    .nsid = 1,      /* namespace 늘어날 일 없음 */
    .addr = (uint64_t)(uintptr_t)send_buf,
    .data_len = send_buf_size,
    .cdw10 = 0, /* input fiemap data의 시작 offset(bytes 단위) */
    .cdw11 = input_bytes, /* output fiemap data의 시작 offset */
    .cdw12 = input->nr_extents,       /* input extents의 수  */
    .cdw13 = output->nr_extents,      /* output extents의 수  */
    .cdw14 = input->total_nr_blocks,  /* 총 input 길이 */
    .cdw15 = output->total_nr_blocks, /* 총 output 길이 */

  };
}

static void deinit_nvme_cmd(struct nvme_passthru_cmd64* cmd) {
  ASSERT(cmd->opcode == 0xD5, "opcode mismatch");
  free((void*)cmd->addr);
  (*cmd) = (struct nvme_passthru_cmd64){0, }; /* zeroing */
}

/* alloc memory with page granularity */
static size_t round_up_with_page(size_t size_bytes) {
  size_t size = ((size_bytes + 4096 - 1) / 4096) * 4096;
  return size;
}

struct llama_tokenization_offload_result* offload_tokenization(const char* nvme_dev, const char* mount_path, const char* input_str, size_t input_len) {

  int nvme_fd = MUST_SUCCESS_POSIX(open(nvme_dev, O_RDWR));
  int mount_fd = MUST_SUCCESS_POSIX(open(mount_path, O_PATH));

  int input_fd = MUST_SUCCESS_POSIX(openat(mount_fd, "prompt.txt", O_WRONLY | O_DIRECT | O_CREAT | O_TRUNC, 0644));
  int output_fd = MUST_SUCCESS_POSIX(openat(mount_fd, "output.bin", O_RDWR | O_DIRECT | O_CREAT | O_TRUNC, 0644));

  void* output_buf = MUST_NONNULL(aligned_alloc(4096, OUTPUT_BUF_SIZE), "output buf alloc failed");

  memset(output_buf, 0, OUTPUT_BUF_SIZE);
  MUST_SUCCESS_POSIX(pwrite(output_fd, output_buf, OUTPUT_BUF_SIZE, 0));
  struct extent_info *output_extent = get_extent_info(output_fd);

  const size_t input_buf_size = round_up_with_page(input_len);
  void* input_buf = MUST_NONNULL(aligned_alloc(4096, input_buf_size), "input buf alloc failed");
  memset(input_buf, 0, input_buf_size);
  memcpy(input_buf, input_str, input_len);

  MUST_SUCCESS_POSIX(pwrite(input_fd, input_buf, input_buf_size, 0));
  struct extent_info *input_extent = get_extent_info(input_fd);
  struct nvme_passthru_cmd64 cmd;
  init_nvme_cmd( input_extent, output_extent, &cmd);

  /* do passthru */
  MUST_SUCCESS_POSIX(nvme_submit_io_passthru64(nvme_fd, &cmd, NULL));

  /* get result */
  MUST_SUCCESS_POSIX(pread(output_fd, output_buf, OUTPUT_BUF_SIZE, 0));

  free(input_extent);
  free(output_extent);

  free(input_buf);
  deinit_nvme_cmd(&cmd);
  close(input_fd);
  close(output_fd);
  close(nvme_fd);
  close(mount_fd);

  return (struct llama_tokenization_offload_result*) output_buf;
}
void llama_tokenization_offload_free(struct llama_tokenization_offload_result* ret) {
  free(ret);
}
