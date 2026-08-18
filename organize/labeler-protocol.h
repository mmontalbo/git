#ifndef LABELER_PROTOCOL_H
#define LABELER_PROTOCOL_H

struct string_list;

/*
 * The labeler protocol. git organize runs the configured labeler and
 * records, in `labeled` (path -> its "k=value ..." string in util), the
 * labels it returns for every file in `scoped_files`. The labeler writes one
 * `path \0 labels \0` record per file.
 */
void run_labeler(const char *cmd, struct string_list *scoped_files,
		 struct string_list *labeled);

#endif /* LABELER_PROTOCOL_H */
