using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using Playerbot.Core.Models;

namespace Playerbot.Core.Contexts;

public class PlayerbotDbContext : DbContext
{
    private readonly IConfiguration _configuration;

    public PlayerbotDbContext(IConfiguration configuration)
    {
        _configuration = configuration;
    }

    public DbSet<PlayerbotScript> Scripts { get; set; } = null!;
    public DbSet<PlayerbotToken> Tokens { get; set; } = null!;

    protected override void OnConfiguring(DbContextOptionsBuilder optionsBuilder)
    {
        var mysqlHost = _configuration["MYSQL_HOST"];
        var mysqlUser = _configuration["MYSQL_USER"];
        var mysqlPass = _configuration["MYSQL_PASS"];

        var connectionString = $@"server={mysqlHost};userid={mysqlUser};password={mysqlPass};database=tbccharacters";

        var serverVersion = new MySqlServerVersion(new Version(5, 7, 37));

        optionsBuilder.UseMySql(connectionString, serverVersion)
            // The following three options help with debugging, but should
            // be changed or removed for production.
            .LogTo(Console.WriteLine, LogLevel.Information)
            .EnableSensitiveDataLogging()
            .EnableDetailedErrors();

        //optionsBuilder.UseMySQL(connectionString);
    }

    protected override void OnModelCreating(ModelBuilder modelBuilder)
    {
        modelBuilder.Entity<PlayerbotScript>().HasKey(s => new { s.Name, s.AccountId });
    }
}
