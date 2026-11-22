"""Independent RIFF parsing and public C/ctypes PCM equivalence for offline examples."""
import argparse,hashlib,json,math,shutil,struct,subprocess,tempfile
from pathlib import Path

HEADER='frame\tdemand\tload_nm\tignition\tstarter\tdriven_rpm\n'
def events(rate,mode):
    edges=[0,1,147,147,160,257,441,rate,rate+147]
    demand=[.25,.3,.2,.35,.25,.25,.25,.25,.25]
    rpm=[0,0,100,200,600,1200,1800,1200,0]
    return [(frame,demand[i],1 if i==2 else 0,1,1 if mode==0 and i<7 else 0,rpm[i] if mode else 0) for i,frame in enumerate(edges)]
def table(rows):return HEADER+''.join('\t'.join(map(str,row))+'\n' for row in rows)
def wave(path,rate,frames):
    data=path.read_bytes();assert data[:4]==b'RIFF' and data[8:12]==b'WAVE'
    assert struct.unpack_from('<I',data,4)[0]==len(data)-8
    chunks={};offset=12
    while offset<len(data):
        name=data[offset:offset+4];length=struct.unpack_from('<I',data,offset+4)[0];offset+=8;assert offset+length<=len(data);assert name not in chunks;chunks[name]=data[offset:offset+length];offset+=length+(length&1)
    assert offset==len(data) and set(chunks)=={b'fmt ',b'fact',b'data'}
    assert struct.unpack('<HHIIHHH',chunks[b'fmt '])==(3,1,rate,rate*4,4,32,0)
    assert struct.unpack('<I',chunks[b'fact'])[0]==frames and len(chunks[b'data'])==frames*4
    samples=struct.unpack('<'+'f'*frames,chunks[b'data']);assert all(math.isfinite(x) and -1<=x<=32767/32768 for x in samples)
    if frames>rate:assert sum(x*x for x in samples)>1e-6
    return chunks[b'data']
def execute(args,out,expected=0):
    p=subprocess.run(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    out.append({'argv':list(map(str,args)),'exit':p.returncode,'output':p.stdout})
    assert (p.returncode==0)==(expected==0),(args,p.stdout)
    return p.stdout

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cli',type=Path,required=True);p.add_argument('--reference',type=Path,required=True);p.add_argument('--fixture',type=Path,required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--ffi',type=Path);a=p.parse_args()
    a.out.mkdir(parents=True,exist_ok=True);work=Path(tempfile.mkdtemp(prefix='render espaço 試験-',dir=a.out));commands=[];measurements=[]
    active=a.fixture
    def invocation(rate,mode,seed,frames,schedule,output):return [a.cli,'--entry',active/'main.mr','--library-root',active/'library','--asset-root',active,'--rate',rate,'--mode','driven' if mode else 'free','--seed',seed,'--frames',frames,'--schedule',schedule,'--out',output]
    try:
        for rate,mode,seed in [(44100,0,1),(48000,0,2**64-1),(48000,1,17)]:
            active=a.fixture
            if seed==2**64-1:
                active=work/'Stochastic modelo 試験';shutil.copytree(a.fixture,active)
                model=active/'main.mr';text=model.read_text();assert 'noise: 0.0, jitter: 0.0' in text
                model.write_text(text.replace('noise: 0.0, jitter: 0.0','noise: 0.25, jitter: 0.2').replace('burning_efficiency_randomness: 0.0','burning_efficiency_randomness: 0.15'))
            frames=rate*2+17;rows=events(rate,mode);schedule=work/f'{rate}-{mode}.tsv';schedule.write_text(table(rows),encoding='utf-8');output=work/f'音声-{rate}-{mode}.wav';raw=work/f'参考-{rate}-{mode}.f32'
            execute(invocation(rate,mode,seed,frames,schedule,output),commands)
            pcm=wave(output,rate,frames)
            execute([a.reference,active/'main.mr',active/'library',active,rate,mode,seed,frames,raw],commands)
            assert pcm==raw.read_bytes(),'CLI PCM differs from independent public C renderer'
            if seed==2**64-1:
                low=work/'truncated-seed.f32'
                execute([a.reference,active/'main.mr',active/'library',active,rate,mode,2**32-1,frames,low],commands)
                assert pcm!=low.read_bytes(),'Seed fixture must distinguish the high32bits'
            if a.ffi:
                from ffi_smoke import render
                direct,telemetry=render(a.ffi,active/'main.mr',(active/'library',active),rate,mode,seed,frames,rows)
                assert direct==pcm,'ctypes PCM differs from native CLI'
            measurements.append({'rate':rate,'mode':mode,'frames':frames,'data_bytes':len(pcm),'sha256':hashlib.sha256(pcm).hexdigest(),'ffi_compared':bool(a.ffi)})
        active=a.fixture
        # UTF-8 BOM/CRLF and a zero-frame file are independently valid.
        s=work/'empty.tsv';s.write_bytes(b'\xef\xbb\xbf'+HEADER.replace('\n','\r\n').encode());o=work/'empty.wav';execute(invocation(44100,0,1,0,s,o),commands);assert wave(o,44100,0)==b''
        bad={
            'bad-header':b'frame,demand,load_nm,ignition,starter,driven_rpm\n',
            'nonmonotonic':table([(2,.2,0,1,1,0),(1,.2,0,1,1,0)]).encode(),
            'outside':table([(44100,.2,0,1,1,0)]).encode(),
            'fractional-frame':(HEADER+'1.5\t.2\t0\t1\t1\t0\n').encode(),
            'overflow-frame':(HEADER+'18446744073709551616\t.2\t0\t1\t1\t0\n').encode(),
            'nan-hidden-by-duplicate':table([(0,'nan',0,1,1,0),(0,.2,0,1,1,0)]).encode(),
            'infinite-load':table([(0,.2,'inf',1,1,0)]).encode(),
            'bad-boolean':table([(0,.2,0,2,1,0)]).encode(),
            'negative-load':table([(0,.2,-1,1,1,0)]).encode(),
            'excess-load':table([(0,.2,10001,1,1,0)]).encode(),
            'excess-demand':table([(0,1.01,0,1,1,0)]).encode(),
            'free-rpm':table([(0,.2,0,1,1,100)]).encode(),
            'missing-field':(HEADER+'0\t.2\t0\t1\t1\n').encode(),
            'extra-field':(HEADER+'0\t.2\t0\t1\t1\t0\t99\n').encode(),
            'invalid-utf8':HEADER.encode()+b'0\t\xff\t0\t1\t1\t0\n',
            'trailing-malformed':(table([(0,.25,0,1,1,0)])+'17\tbad\t0\t1\t1\t0\n').encode(),
        }
        for name,data in bad.items():
            s=work/(name+'.tsv');s.write_bytes(data);o=work/(name+'.wav');execute(invocation(44100,0,1,44100,s,o),commands,1);assert not o.exists() and not list(work.glob(o.name+'.tmp-*'))
        valid=work/'valid.tsv';valid.write_text(table([(0,.25,0,1,1,0)]))
        for frames in [(2**32-50)//4+1,2**64-1]:
            o=work/(str(frames)+'.wav');execute(invocation(44100,0,1,frames,valid,o),commands,1);assert not o.exists()
        o=work/'keep.wav';o.write_bytes(b'original');execute(invocation(44100,0,1,44100,valid,o),commands,1);assert o.read_bytes()==b'original'
        o=work/'absent-parent/out.wav';execute(invocation(44100,0,1,44100,valid,o),commands,1);assert not o.exists()
        # Runtime fault after a valid prefix must still discard the whole temp WAV.
        s=work/'saturate.tsv';s.write_text(table([(0,.25,100,1,0,5000)]));o=work/'fault.wav';message=execute(invocation(48000,1,1,48000,s,o),commands,1);assert 'saturat' in message;assert not o.exists() and not list(work.glob(o.name+'.tmp-*'))
        s=work/'driven-starter.tsv';s.write_text(table([(0,.25,0,1,1,0)]));o=work/'bad-mode.wav';execute(invocation(48000,1,1,48000,s,o),commands,1);assert not o.exists()
        result={'status':'pass','measurements':measurements,'negative_cases':len(bad)+6,'commands':commands}
    except Exception as error:
        result={'status':'fail','error':repr(error),'commands':commands}
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='commands'},indent=2));return 0 if result['status']=='pass' else 1
if __name__=='__main__':raise SystemExit(main())
