#include "stdio.h"
#include "mregexp.h"

int main() {
  // Read the regular expression
  char regex_str[128];
  scanf("%s\n", regex_str);


  // Compile the regular expression
  MRegexp* re = mregexp_compile(regex_str);

  // Check for compilation errors
  if (re == NULL) {
    printf("Error compiling regular expression\n");
    return 1;
  }

  char input_str[128];
  while(scanf("%s", input_str) != -1) {
    MRegexpMatch m;
    if (mregexp_match(re, input_str, &m)) {
	printf("Match\n");
    }
    else {
	printf("No Match\n");
    }
  }

  mregexp_free(re);
  return 0;
}
