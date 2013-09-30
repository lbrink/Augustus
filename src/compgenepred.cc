/**********************************************************************
 * file: compgenepred.cc licence: Artistic Licence, see file
 * LICENCE.TXT or
 * http://www.opensource.org/licenses/artistic-license.php descr.:
 * comparative gene prediction on multiple species authors: Mario
 * Stanke, Alexander Gebauer, Stefanie König
 *
 * date    |   author      |  changes
 * --------|---------------|------------------------------------------
 * 07.03.12| Mario Stanke  | creation of the file
 * 06.10.12|Stefanie König | construction of an OrthoGraph from a set of
 *         |               | orthologous sequences + integration of
 *         |               | the optimization method
 **********************************************************************/

#include "compgenepred.hh"
#include "orthograph.hh"
#include "mea.hh"
#include "genomicMSA.hh"
#include "geneMSA.hh"
#include "orthoexon.hh"
#include "namgene.hh"

#include "contTimeMC.hh"
#include <gsl/gsl_matrix.h>
#include <ctime>

CompGenePred::CompGenePred(){
    if (Constant::Constant::dbaccess.empty()) { // give priority to database in case both exist
        rsa = new MemSeqAccess();
    } else {
        rsa = new DbSeqAccess();
    }
}

void CompGenePred::start(){

    // read in alignment, determine orthologous sequence fragments
    
#ifdef DEBUG
    cout << "reading in the phylogenetic tree" << endl;
#endif
    PhyloTree tree(Constant::treefile);  // has to be initialized before OrthoGraph
    ExonEvo evo;
    vector<double> branchset;
    tree.getBranchLengths(branchset);
    evo.setBranchLengths(branchset);
    evo.computeLogPmatrices();
    OrthoGraph::tree = &tree;
    GeneMSA::setTree(&tree);
    OrthoGraph::numSpecies = OrthoGraph::tree->numSpecies();

#ifdef DEBUG
    cout << "-------------------------------\nparameters phylogenetic model\n-------------------------------" << endl;
    cout << "rate exon loss:\t" << evo.getMu() << endl;
    cout << "rate exon gain:\t" << evo.getLambda() << endl;
    cout << "phylo factor:\t" << evo.getPhyloFactor() <<  "\n-------------------------------" << endl;
#endif

    bool dualdecomp;
    try {
	dualdecomp = Properties::getBoolProperty("/CompPred/dualdecomp");
    } catch (...) {
	dualdecomp = true;
    }
    int maxIterations; // maximum number of dual decomposition iterations
    try {
	maxIterations = Properties::getIntProperty("/CompPred/maxIterations");
    } catch (...) {
	maxIterations = 100;
    }
    double dd_factor; // parameter of the dual decomposition step size function 
    try {
	dd_factor = Properties::getdoubleProperty("/CompPred/dd_factor");
    } catch (...) {
	dd_factor = 15;
    }

    //initialize output files of initial gene prediction and optimized gene prediction
    vector<ofstream*> baseGenes = initOutputFiles(".base"); // equivalent to MEA prediction
    vector<int> base_geneid(OrthoGraph::numSpecies, 1); // gene numbering
    vector<ofstream*> initGenes = initOutputFiles(".init"); // score added to all orthologous exons and penalty added to all non orthologous exons, then global path search repeated
    vector<int> init_geneid(OrthoGraph::numSpecies, 1);
    vector<ofstream*> optGenes = initOutputFiles();  //optimized gene prediction by applying majority rule move
    vector<int> opt_geneid(OrthoGraph::numSpecies, 1);
    vector<ofstream*> sampledExons = initOutputFiles(".sampled_ECs");

    BaseCount::init();
    PP::initConstants();
    NAMGene namgene; // creates and initializes the states
    FeatureCollection extrinsicFeatures; // hints, empty for now, will later read in hints for sequence ranges from database
    SequenceFeatureCollection sfc(&extrinsicFeatures);
    StateModel::readAllParameters(); // read in the parameter files: species_{igenic,exon,intron,utr}_probs.pbl

    // temporary tests of codon rate matrix stuff (Mario)
    double *pi = ExonModel::getCodonUsage();
    CodonEvo codonevo;
    codonevo.setKappa(2);
    codonevo.setPi(pi);
    vector<double> b; // all branch lengths occuring in the tree
    b.push_back(.5); // 50% codon substitutions between D.mel and D.pseudoo., 40% between human and mouse
    codonevo.setBranchLengths(b, 10);
    // codonevo.printBranchLengths();
    
    codonevo.setOmegas(40);
    codonevo.setPrior(1.0);
    //cout << "Omegas, for which substitution matrices are stored:" << endl;
    //codonevo.printOmegas();
    codonevo.computeLogPmatrices();
    
    // gsl_matrix *P = codonevo.getSubMatrixLogP(0.3, 0.25);
    // printCodonMatrix(P);
    GeneMSA::setCodonEvo(&codonevo);
  
    vector<string> speciesNames;
    OrthoGraph::tree->getSpeciesNames(speciesNames);
    rsa->setSpeciesNames(speciesNames);
    GenomicMSA msa(rsa);
    msa.readAlignment(Constant::alnfile);  // reads the alignment
    // rsa->printStats();
    msa.compactify(); // trivial mergers of neighboring alignments
    msa.findGeneRanges(); // nontrivial summary of alignments
  
    //msa.printAlignment("");
    //exit(0);

    GeneMSA::openOutputFiles();
    while (GeneMSA *geneRange = msa.getNextGene()) {
	cout << "processing next gene range:" << endl;
	geneRange->printStats();
        OrthoGraph orthograph;
	vector<AnnoSequence> seqRanges(speciesNames.size());
        for (int s = 0; s < speciesNames.size(); s++) {
            string seqID = geneRange->getSeqID(s);
            if (!seqID.empty()) {
		int start = geneRange->getStart(s); // start, end refer to plus strand
		int end = geneRange->getEnd(s);
		AnnoSequence *as = rsa->getSeq(speciesNames[s], seqID, start, end, geneRange->getStrand(s));
                if (!as) {
                    cerr << "random sequence access failed on " << speciesNames[s] << ", " << seqID << ", " 
			 << start << ", " << end << ", " << endl;
                    break;
                } else {
		    seqRanges[s] = *as; // DNA seqs will be reused when omega is computed
		    
		    // this is needed for IntronModel::dssProb in GenomicMSA::createExonCands
                    namgene.getPrepareModels(as->sequence, as->length); 

		    // identifies exon candidates in the sequence for species s
                    geneRange->createExonCands(s, as->sequence);
                    list<ExonCandidate*> additionalExons = *(geneRange->getExonCands(s));

                    namgene.doViterbiPiecewise(sfc, as, bothstrands); // sampling
                    list<Gene> *alltranscripts = namgene.getAllTranscripts();
                    if (alltranscripts){
                        cout << "building Graph for " << speciesNames[s] << endl;
                        /* build datastructure for graph representation
                         * @stlist : list of all sampled states
                         */
                        list<Status> stlist;
                        if(!alltranscripts->empty()){
                            buildStatusList(alltranscripts, false, stlist);
                        }
                        // build graph
                        orthograph.graphs[s] = new SpeciesGraph(&stlist, as, additionalExons, speciesNames[s], 
								geneRange->getStrand(s), sampledExons[s]);
                        orthograph.graphs[s]->buildGraph();

			//save pointers to transcripts and delete them after gene list is build
                        orthograph.ptrs_to_alltranscripts[s] = alltranscripts; 
                    }
                }
            }
        }
	
	geneRange->printGeneRanges();
	geneRange->printExonCands();
	geneRange->createOrthoExons();
	geneRange->computeOmegas(seqRanges); // omega and number of substitutions is stored as OrthoExon attribute
	seqRanges.clear(); // delete sequences
	geneRange->printOrthoExons(rsa);
	orthograph.all_orthoex = geneRange->getOrthoExons();

	for(list<OrthoExon>::iterator hects = orthograph.all_orthoex.begin(); hects != orthograph.all_orthoex.end();
	    hects++){ //TODO: move this to createOrthoExons()
	    for(size_t pos = 0; pos < OrthoGraph::numSpecies; pos++){
		if(hects->orthoex[pos]==NULL){
		    hects->labels[pos]=2;
		}
		else{
		    Node* node = orthograph.graphs[pos]->getNode(hects->orthoex[pos]);
		    hects->orthonode[pos]=node;
		}
	    }
	}
	 
	orthograph.outputGenes(baseGenes,base_geneid);
	//add score for selective pressure of orthoexons
	orthograph.addScoreSelectivePressure();
	//determine initial path
	orthograph.globalPathSearch();
	orthograph.outputGenes(initGenes,init_geneid);
	  
	if(!orthograph.all_orthoex.empty()){
	    if (dualdecomp){ // optimization via dual decomposition
		vector< list<Gene> *> genelist(OrthoGraph::numSpecies);
		orthograph.dualdecomp(evo,genelist,GeneMSA::geneRangeID-1,maxIterations, dd_factor);
		orthograph.filterGeneList(genelist,optGenes,opt_geneid);
	    } else { // optimization by making small changes (moves)
		orthograph.pruningAlgor(evo);
		orthograph.printCache();
		orthograph.optimize(evo);
		// transfer max weight paths to genes + filter + ouput
		orthograph.outputGenes(optGenes, opt_geneid);
	    }
	}
	delete geneRange;
    }

    GeneMSA::closeOutputFiles();

    closeOutputFiles(initGenes);
    closeOutputFiles(baseGenes);
    closeOutputFiles(optGenes);

}
