#ifndef ORGANIZER_PROTOCOL_H
#define ORGANIZER_PROTOCOL_H

struct organize_plan;
struct strbuf;
struct string_list;

/*
 * The organizer protocol. git organize sends the standing moves to the
 * configured organizer, which may reject moves or return a patch of content
 * edits. run_organizer validates that patch against the plan, fills `patch`
 * with the edits and `claimed` with the moves the organizer renames itself,
 * and records each rejected move on the plan.
 */
void run_organizer(const char *command, struct organize_plan *plan,
		   struct strbuf *patch, struct string_list *claimed);

#endif /* ORGANIZER_PROTOCOL_H */
