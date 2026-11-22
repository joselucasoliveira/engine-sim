#include <engine_sim/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int run(int argc,char **argv) {
    es_error error={{0}};es_config config;es_runtime *r=NULL;FILE *file=NULL;float pcm[257];int result=1;
    uint64_t frames,pos=0;unsigned rate,mode;size_t event=0;uint64_t seed;
    uint64_t edges[9];
    if(argc!=9)return 2;
    rate=(unsigned)strtoul(argv[4],NULL,10);mode=(unsigned)strtoul(argv[5],NULL,10);seed=strtoull(argv[6],NULL,10);frames=strtoull(argv[7],NULL,10);
    edges[0]=0;edges[1]=1;edges[2]=147;edges[3]=147;edges[4]=160;edges[5]=257;edges[6]=441;edges[7]=rate;edges[8]=rate+147;
#define CHECK(call) do {if((call)!=ES_OK)goto done;}while(0)
    CHECK(es_config_init(ES_ABI_V1,sizeof(config),&config,&error));config.output_hz=rate;config.mode=mode;config.seed=seed;
    CHECK(es_create(&config,&r,&error));CHECK(es_load(r,argv[1],argv[2],argv[3],&error));
#ifdef _WIN32
    {int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[8],-1,NULL,0);wchar_t *p=(wchar_t *)malloc((size_t)n*sizeof(wchar_t));if(n<=0||!p){free(p);goto done;}if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[8],-1,p,n)){free(p);goto done;}file=_wfopen(p,L"wb");free(p);}
#else
    file=fopen(argv[8],"wb");
#endif
    if(!file)goto done;
    while(pos<frames) {
        uint32_t written=0,count;
        while(event<9&&edges[event]==pos) {
            const double demand[9]={.25,.3,.2,.35,.25,.25,.25,.25,.25};
            const double speed[9]={0,0,100,200,600,1200,1800,1200,0};
            es_controls c={sizeof(c),ES_ABI_V1,1,mode==ES_FREE&&event<7?1u:0u,demand[event],event==2?1.:0.,mode==ES_DRIVEN?speed[event]:0.};
            CHECK(es_set_controls(r,&c,&error));++event;
        }
        {uint64_t left=(event<9?edges[event]:frames)-pos;if(left>frames-pos)left=frames-pos;count=(uint32_t)(left>257?257:left);}
        CHECK(es_render(r,count,pcm,257,&written,&error));if(written!=count||fwrite(pcm,sizeof(float),count,file)!=count)goto done;pos+=count;
    }
    result=0;
done:
    if(file&&fclose(file))result=1;if(result)fprintf(stderr,"reference: %s\n",error.message);es_destroy(r);return result;
}
#ifdef _WIN32
int wmain(int argc,wchar_t **argv){char *utf8[9]={0};int result=2;if(argc!=9)return result;for(int i=1;i<9;++i){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,NULL,0,NULL,NULL);if(n<=0)goto done;utf8[i]=(char *)malloc((size_t)n);if(!utf8[i]||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,utf8[i],n,NULL,NULL))goto done;}result=run(argc,utf8);done:for(int i=1;i<9;++i)free(utf8[i]);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
