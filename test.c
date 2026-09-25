#include <stdio.h>
#include <string.h>

int main() {
    const char *text = "20,,10";

    const char *current_start = text; // Track the start of the current segment
    const char *next_comma = strchr(current_start, ',');

    while (current_start != NULL) {
        int length;

        if (next_comma != NULL) {
            // Found a comma, calculate length up to the comma
            length = next_comma - current_start;
        } else {
            // No more commas, calculate length to the end of the string
            length = strlen(current_start);
        }

        // Allocate the destination buffer (+1 for null terminator)
        char destination[length + 1];
        strncpy(destination, current_start, length);
        destination[length] = '\0';

        // Print the numeric index relative to the *original* text start
        int original_index = current_start - text;
        printf("Found segment at position %d: \"%s\"\n", original_index, destination);

        // Move to the next segment
        if (next_comma != NULL) {
            current_start = next_comma + 1;       // Start right after the comma
            next_comma = strchr(current_start, ','); // Find the next comma
        } else {
            current_start = NULL; // No more segments left, break loop
        }
    }
    return 0;
}