#include <stdio.h>
#include <stdlib.h>

int main(void) {
  /*#region*/
  const char *input_path = getenv("NESTOR_PLUGIN_INPUT");
  if (!input_path) {
    fprintf(stderr, "Missing NESTOR_PLUGIN_INPUT\n");
    return 1;
  }

  // Open and read the temporary JSON input file
  FILE *f = fopen(input_path, "r");
  if (!f) {
    fprintf(stderr, "Failed to open input file: %s\n", input_path);
    return 1;
  }

  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  // Print success json along with the received arguments
  if (n > 0) {
    printf("{\n  \"plugin_output\": \"success\",\n  \"received_args\": %s\n}\n", buf);
  } else {
    printf("{\n  \"plugin_output\": \"success\",\n  \"received_args\": null\n}\n");
  }
  return 0;
  /*#endregion*/
}
