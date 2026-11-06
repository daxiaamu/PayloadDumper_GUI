#ifndef DUMPER_H
#define DUMPER_H

int dumper_run(const char *payload_path, const char *out_dir,
               const char *partitions, int num_threads,
               int list_only, int metadata_only,
               int kernel_version_only, int arb_only);

#endif
