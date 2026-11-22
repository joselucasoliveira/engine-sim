#include <engine_sim/runtime.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(call) do { es_status result=(call); if(result!=ES_OK) { fprintf(stderr,"%s: %d %s\n",#call,(int)result,error.message); es_destroy(runtime); return 1; } } while(0)
int main(int argc,char **argv) {
    es_runtime *runtime=NULL;
    es_error error;
    es_config config;
    es_controls controls={sizeof(es_controls),ES_ABI_V1,1,1,.25,0,0};
    es_telemetry telemetry;
    float samples[443];
    double energy=0,peak=0;
    unsigned nonzero=0;
    if(argc!=4) return 2;
    if(es_api_version()!=ES_ABI_V1 || (es_capabilities()&(ES_CAP_FREE|ES_CAP_44100|ES_CAP_RESET|ES_CAP_PARALLEL_RENDER))!=(ES_CAP_FREE|ES_CAP_44100|ES_CAP_RESET|ES_CAP_PARALLEL_RENDER)) return 3;
    CHECK(es_config_init(ES_ABI_V1,sizeof(config),&config,&error));
    CHECK(es_create(&config,&runtime,&error));
    CHECK(es_load(runtime,argv[1],argv[2],argv[3],&error));
    CHECK(es_set_controls(runtime,&controls,&error));
    for(unsigned block=0;block<1000;++block) {
        uint32_t written=99;
        if(block==110) { controls.starter=0; CHECK(es_set_controls(runtime,&controls,&error)); }
        samples[0]=samples[442]=12345;
        CHECK(es_render(runtime,441,samples+1,441,&written,&error));
        if(written!=441 || samples[0]!=12345 || samples[442]!=12345) { es_destroy(runtime); return 4; }
        for(unsigned i=1;i<=441;++i) {
            double value=samples[i];
            if(!isfinite(value) || value< -1 || value>32767.0/32768) { es_destroy(runtime); return 5; }
            if(block>110) { energy+=value*value; if(value!=0) ++nonzero; if(fabs(value)>peak) peak=fabs(value); }
        }
    }
    memset(&telemetry,0,sizeof(telemetry));telemetry.size=sizeof(telemetry);telemetry.abi=ES_ABI_V1;
    CHECK(es_get_telemetry(runtime,&telemetry,&error));
    printf("frames=%llu ticks=%llu native=%llu rpm=%.9f energy=%.9f peak=%.9f nonzero=%u\n",
        (unsigned long long)telemetry.frames,(unsigned long long)telemetry.ticks,
        (unsigned long long)telemetry.native_frames,telemetry.measured_rpm,energy,peak,nonzero);
    if(telemetry.state!=ES_READY || telemetry.frames!=441000 || telemetry.ticks!=100000
        || telemetry.native_frames!=441000 || telemetry.cylinders!=1 || telemetry.displacement_m3<=0
        || telemetry.measured_rpm<100 || energy<.000001 || nonzero<10000) { es_destroy(runtime);return 6; }
    CHECK(es_unload(runtime,&error));
    CHECK(es_unload(runtime,&error));
    es_destroy(runtime);es_destroy(NULL);return 0;
}
