# Step 3: fits the scoring-chance model (P(this team scores in the next 10 s)) and saves models/mlp.pkl.
# Prints how it does on held-back replays against two simpler models, and how well its percentages hold up.
import numpy as np, glob, sys
from sklearn.neural_network import MLPClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import roc_auc_score, log_loss
K=21
tr=[];te=[]
for f in sorted(glob.glob('out/samples_*.bin')):
    a=np.fromfile(f,dtype=np.float32).reshape(-1,K+2); n=int(len(a)*0.85); tr.append(a[:n]); te.append(a[n:])
tr=np.vstack(tr); te=np.vstack(te)
Xtr,ytr=tr[:,:K],tr[:,K]; Xte,yte=te[:,:K],te[:,K]
print(len(tr),len(te),'base rate',ytr.mean().round(4))
base=log_loss(yte,np.full(len(yte),ytr.mean()))
def rep(n,p): print(f'{n:8s} auc {roc_auc_score(yte,p):.4f} logloss {log_loss(yte,p):.4f} (base {base:.4f})')
lr=LogisticRegression(max_iter=500).fit(Xtr,ytr); rep('logit',lr.predict_proba(Xte)[:,1])
gb=HistGradientBoostingClassifier(max_iter=300,learning_rate=0.08).fit(Xtr,ytr); rep('gbt',gb.predict_proba(Xte)[:,1])
ml=MLPClassifier(hidden_layer_sizes=(32,16),max_iter=40,early_stopping=True,random_state=0,batch_size=512).fit(Xtr,ytr); p=ml.predict_proba(Xte)[:,1]; rep('mlp',p)
# calibration
for lo,hi in [(0,.02),(.02,.05),(.05,.1),(.1,.2),(.2,.4),(.4,.7),(.7,1.01)]:
    m=(p>=lo)&(p<hi); print(f'  pred {lo:.2f}-{hi:.2f}: n {m.sum():6d} mean pred {p[m].mean() if m.any() else 0:.3f} actual {yte[m].mean() if m.any() else 0:.3f}')
rk=te[:,K+1]
for r in range(6):
    m=rk==r; print('rank',r,'actual',yte[m].mean().round(4),'pred',p[m].mean().round(4))
import pickle; pickle.dump(ml,open('models/mlp.pkl','wb'))
