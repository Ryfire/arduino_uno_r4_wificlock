# Custom scene mix — flashed as SCENE_STYLE=6 (mixed preset).
# Each bucket plays its own intro flavor:
#   1 = classic still      (SCENE_ART)
#   2 = classic animated   (sceneNight..sceneDusk)
#   4 = simple animated    (tools/scene_art_draft.py keyframes)
#   5 = simple still       (tools/scene_art_draft.py stills)
# Picked with tools/preview_all_styles.py -> preview_all.html (click cards,
# copy the result here). tools/generate_simple_scenes.py turns this into
# SCENE_MIX[6] in include/simple_scenes.h at build time.
MIX = {
    "night": 2,
    "sunrise": 2,
    "morning": 2,
    "day": 4,
    "sunset": 2,
    "dusk": 2,
}
