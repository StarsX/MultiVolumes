# MultiVolumes
Authors' implementation of our SIGGRAPH Asia 2021 Technical Communications (Viewport-Resolution Independent Anti-Aliased Ray Marching on Interior Faces in Cube-Map Space) demo III. Fast real-time multiple volumes rendering for external volume textures with mesh occlusion.

[Work graph sample]
Recently, a new optional implementation path using work graph was added. In this path, we have rewritten the shaders of the volume-level processing and cube-map space ray marching together on a work graph. To run the program, the developer mode must be enabled for now.

![MultiVolumes](https://github.com/StarsX/MultiVolumes/blob/main/Doc/Images/SA2021_TC.jpg "rendering result of multiple volumes")

Hot keys:

[F1] show/hide FPS

[A] play/stop animation

[M] show/hide mesh

[O] toggle OIT methods

[W] toggle WorkGraph/ExecuteIndirect

[Space] pause/play animation

Prerequisite: https://github.com/StarsX/XUSG
