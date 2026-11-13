console.log("Dashboard loaded");

const statusArea = document.getElementById('status-area');
const buttons = document.querySelectorAll('.btn');

// Handle standard mouse/click interactions ONLY
buttons.forEach(button => {
    button.addEventListener('click', (e) => {
        
        // Extract the filename from the href for our UI status update
        const fileName = e.currentTarget.getAttribute('href').replace('/', '');
        console.log(`Downloading ${fileName}...`);
        
        // Update the UI to show action
        statusArea.textContent = `Downloading ${fileName}...`;
        
        // Reset status text after 2 seconds
        setTimeout(() => {
            statusArea.textContent = '';
        }, 1000);
    });
});