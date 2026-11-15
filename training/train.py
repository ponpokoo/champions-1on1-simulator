"""Bounded game-balanced Monte Carlo PPO; runs on PC Python 3.10 / torch."""
import argparse,json,time,os
from pathlib import Path
import numpy as np
import torch
from torch import nn
from model import DIMS,NAMES,SHAPES,encode,load_model,save_model,export_header,SCHEMA,ENCODER
from data import read,validation_mask

class Policy(nn.Module):
    def __init__(self,arrays,prior):
        super().__init__()
        for name,a in zip(NAMES,arrays): setattr(self,name,nn.Parameter(torch.from_numpy(a.copy())))
        self.prior=prior
    def forward(self,x,mask,prior):
        h=torch.relu(nn.functional.linear(x,self.w1,self.b1))
        h=torch.relu(nn.functional.linear(h,self.w2,self.b2))
        logits=nn.functional.linear(h,self.wp,self.bp)+nn.functional.one_hot(prior,5)*self.prior
        logits=logits.masked_fill(~mask,-1e9)
        value=torch.tanh(nn.functional.linear(h,self.wv,self.bv)).squeeze(-1)
        return torch.distributions.Categorical(logits=logits),value,logits

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('--data',required=True); p.add_argument('--input',required=True); p.add_argument('--output',required=True)
    p.add_argument('--epochs',type=int,default=6); p.add_argument('--seconds',type=int,default=540); p.add_argument('--cpu-smoke',action='store_true'); p.add_argument('--seed',type=int,default=73103)
    p.add_argument('--lr',type=float,default=3e-4); p.add_argument('--kl-stop',type=float,default=0.03); args=p.parse_args()
    if not torch.cuda.is_available() and not args.cpu_smoke: raise SystemExit('CUDA unavailable: full training refused; use --cpu-smoke')
    if args.epochs<1 or args.seconds<1: raise SystemExit('positive bounds required')
    torch.set_num_threads(4); torch.manual_seed(args.seed); np.random.seed(args.seed)
    torch.backends.cuda.matmul.allow_tf32=False; torch.backends.cudnn.allow_tf32=False
    device=torch.device('cpu' if args.cpu_smoke else 'cuda')
    arrays,prior,digest=load_model(args.input)
    records,obs,masks,info=read(args.data,digest)
    if args.cpu_smoke and info['games']>256: raise SystemExit('CPU smoke accepts at most 256 games; full CPU campaign refused')
    x=torch.from_numpy(encode(obs)).to(device); mask=torch.from_numpy(masks).to(device)
    action=torch.tensor(records['action'].astype(np.int64),device=device); hint=torch.tensor(records['prior'].astype(np.int64),device=device)
    oldlog=torch.tensor(records['logp'].copy(),device=device); oldvalue=torch.tensor(records['value'].copy(),device=device)
    target=torch.tensor(records['score'].astype(np.float32)-1,device=device)
    ids=records['game'].astype(np.uint64); held=validation_mask(ids,args.seed)
    train=torch.tensor(np.flatnonzero(~held),device=device); val=torch.tensor(np.flatnonzero(held),device=device)
    if not len(train) or not len(val): raise SystemExit('need both train and held-out games')
    lengths=np.bincount(records['game']); weights=torch.tensor((len(records)/info['games']/lengths[records['game']]).astype(np.float32),device=device)
    advantage=target-oldvalue
    mean=(advantage[train]*weights[train]).sum()/weights[train].sum()
    std=torch.sqrt(((advantage[train]-mean)**2*weights[train]).sum()/weights[train].sum()+1e-8)
    advantage=(advantage-mean)/std
    net=Policy(arrays,prior).to(device); opt=torch.optim.Adam(net.parameters(),lr=args.lr)
    # Independent C/PyTorch behavior parity before any parameter update.
    with torch.no_grad():
        sample=train[:min(4096,len(train))]; dist,v,_=net(x[sample],mask[sample],hint[sample])
        logerr=float((dist.log_prob(action[sample])-oldlog[sample]).abs().max()); verr=float((v-oldvalue[sample]).abs().max())
        if logerr>2e-4 or verr>2e-4: raise SystemExit('C/Python behavior parity failure '+str((logerr,verr)))
    start=time.monotonic(); history=[]; steps=0
    for epoch in range(min(args.epochs,1) if args.cpu_smoke else args.epochs):
        order=train[torch.randperm(len(train),device=device)]; losses=[]
        for idx in order.split(2048):
            if time.monotonic()-start>args.seconds: break
            dist,v,_=net(x[idx],mask[idx],hint[idx]); logp=dist.log_prob(action[idx]); ratio=(logp-oldlog[idx]).exp()
            pg=-torch.minimum(ratio*advantage[idx],ratio.clamp(.8,1.2)*advantage[idx]); vl=(v-target[idx])**2
            loss=((pg+.5*vl-.01*dist.entropy())*weights[idx]).mean()
            opt.zero_grad(); loss.backward(); nn.utils.clip_grad_norm_(net.parameters(),.5); opt.step(); losses.append(float(loss)); steps+=1
        with torch.no_grad():
            sums=np.zeros(6); total=0; decisive=0; correct=0
            for idx in val.split(4096):
                dist,v,_=net(x[idx],mask[idx],hint[idx]); logp=dist.log_prob(action[idx]); ratio=(logp-oldlog[idx]).exp(); wt=weights[idx]; total+=float(wt.sum())
                sums+=np.array([float(((v-target[idx])**2*wt).sum()),float((dist.entropy()*wt).sum()),float(((ratio-1-(logp-oldlog[idx]))*wt).sum()),float(((ratio-1).abs().gt(.2)*wt).sum()),float((-logp*wt).sum()),0])
                dec=target[idx]!=0; decisive+=float(wt[dec].sum()); correct+=float(wt[dec][(v[dec]>0)==(target[idx][dec]>0)].sum())
            row=dict(epoch=epoch+1,steps=steps,loss=float(np.mean(losses)) if losses else None,value_mse=sums[0]/total,entropy=sums[1]/total,approx_kl=sums[2]/total,clip_fraction=sums[3]/total,action_nll=sums[4]/total,value_decisive_accuracy=correct/decisive if decisive else None,seconds=time.monotonic()-start)
        history.append(row); print(json.dumps(row),flush=True)
        if time.monotonic()-start>=args.seconds or row['approx_kl']>args.kl_stop: break
    out=Path(args.output); out.parent.mkdir(parents=True,exist_ok=True)
    final=[getattr(net,n).detach().cpu().numpy() for n in NAMES]; newhash=save_model(out,final,prior); export_header(out,str(out)+'.h')
    with torch.no_grad():
        ix=np.linspace(0,len(records)-1,min(256,len(records)),dtype=int); it=torch.tensor(ix,device=device)
        _,v,logits=net(x[it],mask[it],hint[it])
        np.savez_compressed(str(out)+'.parity.npz',obs=obs[ix],mask=masks[ix],prior=records['prior'][ix],logits=logits.cpu().numpy(),value=v.cpu().numpy())
    report=dict(schema=SCHEMA,encoder=ENCODER,input_hash=digest,weights_sha256=newhash,data=info,train_games=int(np.unique(ids[~held]).size),validation_games=int(np.unique(ids[held]).size),torch=torch.__version__,cuda_available=torch.cuda.is_available(),device=str(device),device_name=torch.cuda.get_device_name(0) if device.type=='cuda' else 'CPU',cpu_smoke=args.cpu_smoke,epochs=history,seconds=time.monotonic()-start,behavior_logp_max_error=logerr,behavior_value_max_error=verr)
    Path(str(out)+'.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report),flush=True)
