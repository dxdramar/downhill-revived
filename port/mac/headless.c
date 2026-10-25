/* What the Mac build doesn't have yet (finding 075): texture packs (texpack.c decodes PNGs with
   Windows' WIC) and the updater (update.c). */
#include "hle.h"
#include "../gs/gs.h"
#include <d3d11.h>
#include "../gs/texpack.h"

#include <stdlib.h>

int texpack_open(ID3D11Device *device) { (void)device; return 0; }
int texpack_available(void) { return 0; }
TexPackEntry *texpack_find(const Gs *g, unsigned ctxt) { (void)g; (void)ctxt; return NULL; }
ID3D11ShaderResourceView *texpack_view(TexPackEntry *e, uint64_t field) { (void)e; (void)field; return NULL; }
void texpack_field(uint64_t field) { (void)field; }
void texpack_report(void) {}

/* No updater on the Mac yet (port/hle/update.c). */
void update_save_environment(void) {}
void update_at_start(void) {}
int update_after_game(int code) { return code; }
void update_request(void) {}
